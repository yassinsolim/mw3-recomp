#include "texture_cache.h"
#include "../../diagnostics.h"

#include "pipeline.h"
#include "recorder.h"
#include "texture_formats.h"
#include "bc_decoder.h"
#include "../texture.h"
#include "../texture_replace.h"
#include "../memory_watch.h"
#include "renderer.h"
#include "../../crash.h"
#include "../../env.h"
#include "../../guest.h"
#include "../../report.h"
#include "../../image_move.h"
#include "../../kernel/physical.h"
#include "../../log.h"
#include "../../platform.h"
#include "../../stutters.h"
#include "../../words_hash.h"

#ifndef MW2_HAVE_VULKAN
// No loader, so nothing here can be built. Every caller tolerates a cache that
// never comes up, because that is also what happens on a machine with no GPU.
bool     vk::textures::Initialise() { return false; }
void     vk::textures::Shutdown() {}
bool     vk::textures::Ready() { return false; }
uint64_t vk::textures::Upload(const gpu::TextureFetch&, const char** error)
{
    if (error) *error = "built without Vulkan";
    return kNone;
}
uint64_t vk::textures::UploadData(const gpu::TextureData&, uint32_t, const char** error)
{
    if (error) *error = "built without Vulkan";
    return kNone;
}
uint64_t vk::textures::Adopt(uint64_t, void*, const gpu::TextureFetch&, uint32_t, uint32_t, uint32_t) { return kNone; }
void vk::textures::ForgetViews() {}
uint32_t vk::textures::Handle(uint64_t, uint8_t, const float**) { return 0; }
void* vk::textures::Set() { return nullptr; }
bool  vk::textures::Readback(uint64_t, std::vector<uint8_t>&, const char** error)
{
    if (error) *error = "built without Vulkan";
    return false;
}
uint64_t vk::textures::NoteDrawnInWorld(uint64_t id) { return id; }
void vk::textures::NewFrame() {}
void vk::textures::Report() {}
void* vk::textures::EndSegment() { return nullptr; }
void* vk::textures::EndSubmission() { return nullptr; }
uint32_t vk::textures::Version(uint64_t) { return 0; }
bool vk::textures::BoundInThisSegment(uint32_t, uint32_t) { return false; }
void vk::textures::TakeLateRestages(std::vector<uint64_t>& ids) { ids.clear(); }
uint64_t vk::textures::FrameNumber() { return 0; }
uint64_t vk::textures::LiveBytes() { return 0; }
uint64_t vk::textures::Budget() { return 0; }
void vk::textures::SetBudget(uint64_t) {}
void vk::textures::MemoryWritten(uint32_t, uint32_t) {}
uint64_t vk::textures::Epoch() { return 0; }
void vk::textures::BeginUploads(uint32_t) {}
#else

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <vulkan/vulkan.h>
#include "util.h"
#include "image_memory.h"

namespace
{
    // MW2_FLASH_HUNT=1 (tools/flash_hunt.sh): every texture that reaches the
    // screen with nothing in it gets its line, not only the first few, so each
    // can be matched with a flash somebody saw.
    bool FlashHunt()
    {
        static const bool on = diag::Flag("MW2_FLASH_HUNT");
        return on;
    }

    // What the decoder writes into. It writes into `dst.bytes` itself, so
    // handing it this same object again reuses that buffer: a restage of a
    // texture whose size has not changed then pays neither the allocation nor
    // the zero fill of the whole decoded image, which measured as 40% of a
    // 2048x2048 DXT1 decode (5.64 ms down to 3.39 ms for the same bytes).
    //
    // Its own thread, because the command stream has one owner and a second
    // decoding thread must not wait on the first. The largest decode seen stays
    // resident until a smaller budget says the memory is wanted back
    // (SetBudget), which is what a phone's onTrimMemory arrives as. The
    // re-reads of a segment's end that run side by side each decode into a
    // buffer of their own, of which this is the first (PrepareAll).
    gpu::TextureData& DecodeScratch()
    {
        static thread_local gpu::TextureData scratch;
        return scratch;
    }

    void ReleaseScratch(gpu::TextureData& scratch)
    {
        std::vector<uint8_t>().swap(scratch.bytes);
        scratch.levels.clear();
        scratch.ok = false;
    }

    // Whether this texture has to go through the software decoder rather than
    // the driver's own BC sampling.
    //
    // Hardware BC is off altogether on Adreno (pipeline.cpp), so there every BC
    // format lands here -- at eight times the bytes for BC1 and four for the
    // rest, which is the workaround's cost. MW2_ADRENO_BC=narrow keeps the
    // driver's sampling and software-decodes only the BC1 family, the case its
    // sampler is known to get wrong. MW2_FORCE_BC_DECOMPRESS makes a clean
    // desktop driver exercise the same decoder, which is how a decode bug is
    // told apart from a driver one.
    bool DecodeInSoftware(uint32_t xenosFormat)
    {
        if (!vk::bc::IsBCFormat(xenosFormat)) return false;
        static const bool force = env::Flag("MW2_FORCE_BC_DECOMPRESS");
        if (force) return true;
        if (!vk::pipeline::TextureCompressionBC()) return true;
        if (vk::bc::AdrenoMode() != vk::bc::AdrenoBC::kDxt1Only) return false;
        // 18 is DXT1 and 51 its 16_16_16_16 alias: the two that carry a 1-bit
        // alpha mode, and the sizes the driver samples wrong for a frame.
        return xenosFormat == 18 || xenosFormat == 51;
    }

    // Whether the image made for this fetch constant is an sRGB one: the
    // title asks for the stored value to be linearised, and the format it is
    // uploaded in has such a form.
    bool SampledAsGamma(const gpu::TextureFetch& fetch)
    {
        return fetch.Linearises() &&
               vk::formats::SrgbFormatFor(vk::formats::VulkanFormatFor(fetch.Format())) !=
                   VK_FORMAT_UNDEFINED;
    }

    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        vk::imagememory::Range memory;
        VkImageView view = VK_NULL_HANDLE;
        // Borrowed from the sampler cache; what an id that names no sampler
        // of its own is sampled with, and its place in the bindless set.
        VkSampler sampler = VK_NULL_HANDLE;
        uint32_t samplerSlot = 0;
        // The view the bindless set holds at this id; null when it holds none
        // that is still good.
        VkImageView written = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0, height = 0;
        // One over the extent of level 0 each way, the third a volume's
        // depth: what Handle gives a pixel shader that takes its textures'
        // sizes from the draw's record (shader::PixelArena::Texels). Set
        // where the image is made (MakeImage) or its view borrowed (Adopt).
        float perTexel[3] = { 1.0f, 1.0f, 1.0f };
        uint32_t blocksWide = 0, blocksHigh = 0, bytesPerBlock = 0;
        uint32_t blockWidth = 1, blockHeight = 1;
        uint32_t swizzle = 0x688;             // XYZW, the identity
        VkDeviceSize bytes = 0;
        // Recorded, but a 1D texture is still made a 2D image one row tall: the
        // translator samples every 1D fetch as 2D at v = 0, because the title
        // also reads 2D textures along a line through 1D fetches, and a module
        // and view that disagree on the dimension are invalid either way round.
        bool oneDimensional = false;
        // Six for a cube map, which is a real cube: the shader turns the face and
        // two coordinates the title computed back into a direction, so the
        // hardware picks the level from a direction that does not jump at the
        // face edges. For a volume the same count is the depth, and the image is
        // a real 3D one -- an array bound where the module says 3D samples as
        // zero.
        uint32_t layers = 1;
        bool volume = false;
        // A title that rewrites a texture in place leaves its fetch constant
        // alone, so the cache key cannot see it. The signature can.
        uint64_t signature = 0;
        uint64_t checkedFrame = ~uint64_t(0);
        // How many frames to leave before signing this one again. A texture
        // that has not moved in a while is watched less and less, because
        // signing every bound texture every frame cost ten seconds of a ninety
        // second run. One the guest has rewritten even once is watched every
        // frame from then on: the title rewrites its model-lighting table in
        // place whenever it hands out new patches.
        uint64_t signPeriod = 1;
        uint64_t queuedSegment = ~0ull;   // the segment it was last put on rewrittenThisSegment
        // When its memory is watched (gpu/memory_watch.h), the stamp it was read
        // under: while no page of it has been written since, it is unchanged
        // for certain and nothing need be signed. Zero when it is not watched.
        uint64_t watchStamp = 0;
        // For a texture read in parts (ReadInParts): the boxes of blocks each
        // page of its memory holds, made the first time they are wanted, and
        // the pages written since the image was last staged -- or, with
        // `partsKnown` false, that those are not known and it is read whole.
        std::vector<gpu::TextureBox> pageBoxes;
        bool boxesAsked = false;
        std::vector<uint32_t> writtenPages;
        bool partsKnown = false;
        uint32_t staged = 0;   // times staged; the draw list's version of it
        uint64_t boundSegment = ~0ull;   // the last segment a draw of which bound it
        bool rewritten = false;
        uint32_t address = 0, xenosFormat = 0;   // physical
        // Found rewritten at a bind while uploads are open: the read is left
        // to the end of the segment (RefreshRewritten), which looks at it again
        // anyway and whose staging is the one the segment's draws see.
        // `dueSignature` is what its memory signed as when that was found.
        bool restageDue = false;
        uint64_t dueSignature = 0;
        // The mip chain's memory, signed together with the base's; zero for a
        // texture with no chain.
        uint32_t mipAddress = 0;
        size_t mipBytes = 0;
        // Where each level is in the staged bytes. One level for anything
        // that did not come from a fetch constant with a chain.
        std::vector<gpu::TextureData::Level> levels;
        uint32_t fetchWords[6]{};
        // Every fetch constant that names this image: the same texture under
        // each sampler state it has been bound with.
        std::vector<std::array<uint32_t, 6>> names;
        std::array<uint32_t, 6> content{};   // its key in State::byContent
        uint32_t minLevel = 0;               // the lowest level it holds a picture for
        // Read from a block that looked never written, and when that was first
        // seen; read again at the end of its segment (EndSegment).
        bool fromAnEmptyBlock = false, countedDrawn = false;
        uint64_t emptySince = 0;
        size_t sourceBytes = 0;
        bool borrowed = false;                // the view belongs to someone else
        bool pinned = false;                  // never evicted: the magenta marker
        // The key of the replacement the image holds in place of the title's
        // bytes (gpu/texture_replace.h); zero for the title's own.
        uint64_t skin = 0;
        // The frame of that replacement the image shows, when it is an
        // animation (Animate); kStill for a single picture.
        uint32_t frame = gpu::replace::kStill;
        // Nothing names it any more (Orphan): it stays as it is for the draws
        // already recorded with it, and goes once idle.
        bool orphaned = false;
        uint64_t lastBound = 0;               // the frame, for eviction
    };

    // Sixteen frames is a quarter of a second at sixty: long enough to be cheap,
    // short enough that a texture that starts moving is caught before it shows.
    constexpr uint64_t kSignPeriodMax = 16;

    struct StagingBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    // The uploads of one renderer slot, and the staging they read from: a
    // command buffer for each segment of the submission (EndSegment), each
    // submitted just ahead of its segment's draws. A submission's uploads are
    // sub-allocated from one buffer, grown for the next time round when they
    // overflow it; what overflows gets a buffer of its own until then.
    struct UploadSlot
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> commands;   // made as segments need them
        size_t segment = 0;                      // the one being recorded into
        bool recording = false;
        StagingBuffer staging;
        VkDeviceSize used = 0, wanted = 0;
        std::vector<StagingBuffer> overflow;
    };
    constexpr VkDeviceSize kStagingLeast = 8ull << 20, kStagingMost = 256ull << 20;

    struct State
    {
        VkDevice device = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        VkPhysicalDeviceMemoryProperties memory{};
        uint32_t maxDimension = 0;

        VkCommandPool commands = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        UploadSlot uploadSlots[vk::textures::kUploadSlots];
        UploadSlot* open = nullptr;   // uploads open between BeginUploads and EndSubmission
        uint64_t uploadsBatched = 0, uploadsAlone = 0, stagingOverflows = 0;
        // Set 1: every image two past its id, after the stand-ins, every
        // sampler at its slot (bindings.h).
        VkDescriptorPool bindlessPool = VK_NULL_HANDLE;
        VkDescriptorSet bindless = VK_NULL_HANDLE;

        uint64_t frame = 0;
        uint64_t refreshed = 0, resigned = 0, invalidatedByWrites = 0;
        uint64_t signNanos = 0;                       // spent signing, all textures
        uint64_t unchangedByWatch = 0;   // checks the write watch answered without signing

        // The two lists read again at the end of the segment (EndSegment):
        // textures uploaded from a block that looked empty, and textures the
        // guest has been seen to rewrite and a draw of the segment binds. Our
        // parse runs ahead of where the console's GPU would be, and that GPU
        // reads a texture when the draw executes, so a texture signed when a
        // draw binds it can still change before the draw would have run.
        std::vector<uint64_t> emptyThisSegment, rewrittenThisSegment;
        std::vector<uint64_t> lateRestages;   // see TakeLateRestages
        uint64_t segment = 0;                 // EndSegment calls so far
        uint64_t epoch = 0;                   // vk::textures::Epoch
        uint64_t segmentsEnded = 0, segmentsWithUploads = 0;
        bool endingSubmission = false;   // inside EndSubmission
        uint64_t rereadRewritten = 0, changedAfterFirstBind = 0;
        // Re-reads that took the written pages' boxes (BoxesToRead): how
        // many, the bytes they staged and the bytes of the whole textures.
        uint64_t stagedInParts = 0, partBytes = 0, wholeBytes = 0;
        // A texture re-read at a segment's end because its memory changed
        // after its draws were parsed, and then never bound again: the title
        // freed it and put something else there. Read before the command
        // processor reports those draws done, that is still the texture the
        // draws were made with; read after, it is whatever replaced it.
        struct LateRead { uint64_t id, frame; };
        std::vector<LateRead> lateReads;
        uint64_t lateReadsBoundAgain = 0, lateReadsOnLastUse = 0;
        uint64_t emptyDrawnInWorld = 0, refilledBeforeSubmit = 0, stillEmptyAtSubmit = 0;

        // MW2_MARK_EMPTY=1: the id of a flat magenta texture, bound in place of
        // anything uploaded from a block with nothing in it.
        uint64_t magenta = 0;
        uint64_t markedEmpty = 0;
        uint64_t linearisedRefused = 0;   // no sRGB form of the format to linearise with
        // Ids are one-based indices into this, so zero is never a texture.
        std::vector<Image> images;
        // A fetch constant names a texture and how it is sampled. byFetch has
        // the id for all six dwords, which is the image and the sampler;
        // byContent has the image for the dwords less the sampler's bits
        // (ContentOf), so a texture bound under several sampler states is
        // held once.
        std::unordered_map<std::array<uint32_t, 6>, uint64_t, WordsHash> byFetch;
        std::unordered_map<std::array<uint32_t, 6>, uint64_t, WordsHash> byContent;
        // In front of byFetch, which was 3.6% of the ring consumer on its own:
        // a segment asks for the same few hundred constants over and over, so
        // most lookups are answered here by one multiply and a compare of the
        // six dwords. An entry is good while `fetchGeneration` is the one it
        // was written under; that moves whenever byFetch loses an entry
        // (Release, Shutdown), the only way a key's id there ever changes.
        struct RecentFetch
        {
            std::array<uint32_t, 6> key{};
            uint64_t id = 0, generation = 0;
        };
        static constexpr size_t kRecentFetches = 1024;
        RecentFetch recentFetches[kRecentFetches];
        uint64_t fetchGeneration = 1;
        std::vector<uint64_t> freeIds;                // slots of g.images to use again
        // A sampler and its slot in the bindless set.
        struct Sampler { VkSampler sampler = VK_NULL_HANDLE; uint32_t slot = 0; };
        std::vector<Sampler> samplerList;             // an id's sampler, by its index less one
        std::map<uint64_t, uint32_t> samplerIndex;    // sampler key -> that index
        std::map<uint64_t, uint64_t> adopted;         // caller's key -> id
        std::map<uint64_t, Sampler> samplers;
        // Images nothing names any more, waiting to be destroyed a few a frame.
        std::deque<Image> doomed;
        Image fallback;
        Image fallbackCube, fallback3D;   // for slots declared as a cube map, a volume
        uint64_t kindMismatches = 0;

        uint32_t uploads = 0, failures = 0;
        uint64_t descriptorsWritten = 0;
        uint64_t uploadedBytes = 0;
        uint64_t liveBytes = 0, budget = 0;   // what the uploaded images hold, and may
        uint64_t evicted = 0, evictedBytes = 0, evictionPasses = 0;
        uint64_t stale = 0, staleBytes = 0;   // released because their memory was rewritten
        uint64_t staleNotReally = 0;          // kept: a page was written, their bytes were not
        uint64_t shared = 0;                  // fetch constants answered by an image already held
        std::map<std::string, uint32_t> failureReasons;
        std::string lastError;
    };
    State g;

    const char* Explain(VkResult r)
    {
        switch (r)
        {
        case VK_SUCCESS: return "ok";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "out of device memory";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "descriptor pool full";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "format not supported";
        default: return "failed";
        }
    }

    bool Fail(const char* what, const char** error)
    {
        g.lastError = what;
        g.failures++;
        g.failureReasons[g.lastError]++;
        if (error) *error = g.lastError.c_str();
        return false;
    }

    // An upload outside a submission's segments, and a read-back: each records,
    // submits and waits on its own.
    VkCommandBuffer BeginCommands()
    {
        // After everything the frames before it have queued, as it was when
        // they were submitted where they were recorded.
        vk::record::Drain();
        vkResetCommandPool(g.device, g.commands, 0);
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(g.command, &begin) != VK_SUCCESS) return VK_NULL_HANDLE;
        return g.command;
    }

    bool EndCommands()
    {
        if (vkEndCommandBuffer(g.command) != VK_SUCCESS) return false;
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &g.command;
        {
            std::lock_guard lock(vk::pipeline::QueueMutex());
            vkResetFences(g.device, 1, &g.fence);
            if (vk::pipeline::Failed(vkQueueSubmit(g.queue, 1, &submit, g.fence),
                                     "a texture upload"))
                return false;
        }
        return !vk::pipeline::Failed(vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX),
                                     "a texture upload's fence");
    }

    bool MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                    VkBuffer& buffer, VkDeviceMemory& memory)
    {
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(g.device, &info, nullptr, &buffer) != VK_SUCCESS) return false;

        VkMemoryRequirements needs{};
        vkGetBufferMemoryRequirements(g.device, buffer, &needs);
        const uint32_t type = vk::util::FindMemory(g.memory, needs.memoryTypeBits, want);
        if (type == UINT32_MAX) { vkDestroyBuffer(g.device, buffer, nullptr); buffer = VK_NULL_HANDLE; return false; }

        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type;
        if (vkAllocateMemory(g.device, &allocate, nullptr, &memory) != VK_SUCCESS)
        {
            vkDestroyBuffer(g.device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        if (vkBindBufferMemory(g.device, buffer, memory, 0) != VK_SUCCESS)
        {
            vkFreeMemory(g.device, memory, nullptr);
            memory = VK_NULL_HANDLE;
            vkDestroyBuffer(g.device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    void DestroyImage(Image& image);

    bool MakeImage(Image& out, const char** error)
    {
        // Made by hand rather than read from a fetch constant: all of it is the
        // one level.
        if (out.levels.empty())
        {
            gpu::TextureData::Level only;
            only.width = out.width;
            only.height = out.height;
            only.depth = out.volume ? out.layers : 1;
            only.blocksWide = out.blocksWide;
            only.blocksHigh = out.blocksHigh;
            only.layers = out.layers;
            only.size = size_t(out.bytes);
            out.levels.push_back(only);
        }
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(g.physical, out.format, &properties);
        // A driver with no BC support would sample garbage rather than refuse.
        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            return Fail("device cannot sample this format", error);

        const bool cube = !out.volume && out.layers == 6;
        if (cube && out.width != out.height) return Fail("a cube map that is not square", error);

        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        if (cube) info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        // A 1D texture is a 2D image one row tall, which is how the translator
        // declares every 1D fetch -- see the note on oneDimensional.
        info.imageType = out.volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        info.format = out.format;
        info.extent = { out.width, out.height, out.volume ? out.layers : 1 };
        out.perTexel[0] = 1.0f / float(info.extent.width);
        out.perTexel[1] = 1.0f / float(info.extent.height);
        out.perTexel[2] = 1.0f / float(info.extent.depth);
        info.mipLevels = uint32_t(std::max<size_t>(out.levels.size(), 1));
        info.arrayLayers = out.volume ? 1 : out.layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_SRC only so an upload can be read back and compared; it costs
        // nothing and is the only way to prove the round trip.
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vkCreateImage(g.device, &info, nullptr, &out.image);
        if (r != VK_SUCCESS) return Fail(Explain(r), error);

        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(g.device, out.image, &needs);
        const uint32_t type = vk::util::FindMemory(g.memory, needs.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX)
        {
            DestroyImage(out);
            return Fail("no device-local memory", error);
        }

        out.memory = vk::imagememory::Allocate(g.device, needs, type);
        if (!out.memory)
        {
            DestroyImage(out);
            return Fail(Explain(VK_ERROR_OUT_OF_DEVICE_MEMORY), error);
        }
        r = vkBindImageMemory(g.device, out.image, out.memory.memory, out.memory.offset);
        if (r != VK_SUCCESS)
        {
            DestroyImage(out);
            return Fail(Explain(r), error);
        }

        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = out.image;
        view.viewType = out.volume ? VK_IMAGE_VIEW_TYPE_3D
                      : cube       ? VK_IMAGE_VIEW_TYPE_CUBE
                                   : VK_IMAGE_VIEW_TYPE_2D;
        view.format = out.format;
        // The fetch constant's swizzle is not decoration: a font atlas whose glyphs
        // are in one channel is read through a swizzle that puts that channel where
        // the shader expects it, so without this the text samples as zero.
        //
        // Because the untiler undoes the fetch constant's own endianness, a guest
        // component lands in the host channel of the same index -- so once the
        // swizzle has been read through the host format's own (see UploadOne) it
        // maps straight onto Vulkan's.
        auto component = [](uint32_t selector) {
            switch (selector & 7)
            {
            case 0:  return VK_COMPONENT_SWIZZLE_R;
            case 1:  return VK_COMPONENT_SWIZZLE_G;
            case 2:  return VK_COMPONENT_SWIZZLE_B;
            case 3:  return VK_COMPONENT_SWIZZLE_A;
            case 4:  return VK_COMPONENT_SWIZZLE_ZERO;
            case 5:  return VK_COMPONENT_SWIZZLE_ONE;
            default: return VK_COMPONENT_SWIZZLE_IDENTITY;
            }
        };
        view.components = { component(out.swizzle >> 0), component(out.swizzle >> 3),
                            component(out.swizzle >> 6), component(out.swizzle >> 9) };
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                  out.volume ? 1u : out.layers };
        r = vkCreateImageView(g.device, &view, nullptr, &out.view);
        if (r != VK_SUCCESS)
        {
            DestroyImage(out);
            return Fail(Explain(r), error);
        }
        return true;
    }

    void DestroyImage(Image& image)
    {
        if (image.borrowed) { image = Image{}; return; }
        if (image.view) vkDestroyImageView(g.device, image.view, nullptr);
        if (image.image) vkDestroyImage(g.device, image.image, nullptr);
        vk::imagememory::Free(g.device, image.memory);
        image = Image{};
    }

    // One region a level, from the bytes staged at `offset`. `bufferRowLength`
    // is in texels even for a block format, so it is the block count times the
    // block width, which is what the untiler produces.
    std::vector<VkBufferImageCopy> Regions(const Image& image, VkDeviceSize offset)
    {
        std::vector<VkBufferImageCopy> regions;
        for (uint32_t level = 0; level < image.levels.size(); level++)
        {
            const gpu::TextureData::Level& l = image.levels[level];
            VkBufferImageCopy region{};
            region.bufferOffset = offset + l.offset;
            region.bufferRowLength = l.blocksWide * image.blockWidth;
            region.bufferImageHeight = l.blocksHigh * image.blockHeight;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0,
                                        image.volume ? 1u : image.layers };
            region.imageExtent = { l.width, l.height, image.volume ? l.depth : 1u };
            regions.push_back(region);
        }
        return regions;
    }

    bool MakeStaging(StagingBuffer& out, VkDeviceSize size)
    {
        if (!MakeBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        out.buffer, out.memory))
            return false;
        void* mapped = nullptr;
        if (vkMapMemory(g.device, out.memory, 0, size, 0, &mapped) != VK_SUCCESS)
        {
            vkDestroyBuffer(g.device, out.buffer, nullptr);
            vkFreeMemory(g.device, out.memory, nullptr);
            out = StagingBuffer{};
            return false;
        }
        out.mapped = static_cast<uint8_t*>(mapped);
        out.size = size;
        return true;
    }

    void FreeStaging(StagingBuffer& staging)
    {
        if (staging.buffer) vkDestroyBuffer(g.device, staging.buffer, nullptr);
        if (staging.memory) vkFreeMemory(g.device, staging.memory, nullptr);
        staging = StagingBuffer{};
    }

    // The copy and the barriers either side of it. The image's earlier
    // contents are not kept, but its earlier readers -- a submission still
    // running, or the draws of this one before it -- are waited for.
    void RecordCopy(VkCommandBuffer cmd, Image& image, VkBuffer source, VkDeviceSize offset)
    {
        constexpr VkPipelineStageFlags kShaders =
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        vk::util::Barrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          kShaders, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const std::vector<VkBufferImageCopy> regions = Regions(image, offset);
        vkCmdCopyBufferToImage(cmd, source, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               uint32_t(regions.size()), regions.data());
        vk::util::Barrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, kShaders);
    }

    // Room for `size` bytes in the slot's staging, in the command buffer of
    // its open segment, which is begun if it has not been: where to write
    // them, and the buffer and offset a copy command names. Null when there
    // is none to be had.
    uint8_t* StagingFor(UploadSlot& slot, size_t size, VkBuffer& buffer, VkDeviceSize& offset,
                        const char** error)
    {
        if (!slot.recording)
        {
            // The pool is this thread's alone, so a segment's buffer is made
            // the first time a submission has that many.
            if (slot.segment >= slot.commands.size())
            {
                VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                allocate.commandPool = slot.pool;
                allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocate.commandBufferCount = 1;
                VkCommandBuffer made = VK_NULL_HANDLE;
                if (vkAllocateCommandBuffers(g.device, &allocate, &made) != VK_SUCCESS)
                {
                    Fail("no upload command buffer", error);
                    return nullptr;
                }
                slot.commands.push_back(made);
            }
            VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(slot.commands[slot.segment], &begin) != VK_SUCCESS)
            {
                Fail("no upload command buffer", error);
                return nullptr;
            }
            slot.recording = true;
        }
        // Offsets a multiple of sixteen suit every block size uploaded.
        const VkDeviceSize at = (slot.used + 15) & ~VkDeviceSize(15);
        slot.wanted = ((slot.wanted + 15) & ~VkDeviceSize(15)) + size;
        StagingBuffer* into = &slot.staging;
        offset = at;
        if (!slot.staging.buffer || at + size > slot.staging.size)
        {
            g.stagingOverflows++;
            slot.overflow.emplace_back();
            if (!MakeStaging(slot.overflow.back(), size))
            {
                slot.overflow.pop_back();
                Fail("no staging buffer", error);
                return nullptr;
            }
            into = &slot.overflow.back();
            offset = 0;
        }
        else slot.used = at + size;
        buffer = into->buffer;
        return into->mapped + offset;
    }

    bool RecordUpload(UploadSlot& slot, Image& image, const uint8_t* bytes, size_t size,
                      const char** error)
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint8_t* staged = StagingFor(slot, size, buffer, offset, error);
        if (!staged) return false;
        std::memcpy(staged, bytes, size);
        RecordCopy(slot.commands[slot.segment], image, buffer, offset);
        g.uploadsBatched++;
        return true;
    }

    // The boxes of a texture read in parts, over what the image holds: its
    // other texels stay, so the layout it is read in is the one it comes from.
    void RecordBoxCopies(VkCommandBuffer cmd, Image& image, VkBuffer source, VkDeviceSize offset,
                         const std::vector<gpu::TextureBox>& boxes,
                         const std::vector<size_t>& offsets)
    {
        constexpr VkPipelineStageFlags kShaders =
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        vk::util::Barrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, kShaders, VK_PIPELINE_STAGE_TRANSFER_BIT);
        std::vector<VkBufferImageCopy> regions(boxes.size());
        for (size_t i = 0; i < boxes.size(); i++)
        {
            const gpu::TextureBox& box = boxes[i];
            VkBufferImageCopy& region = regions[i];
            region.bufferOffset = offset + offsets[i];
            region.bufferRowLength = box.w;
            region.bufferImageHeight = box.h;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageOffset = { int32_t(box.x), int32_t(box.y), int32_t(box.z) };
            region.imageExtent = { box.w, box.h, box.d };
        }
        vkCmdCopyBufferToImage(cmd, source, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               uint32_t(regions.size()), regions.data());
        vk::util::Barrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, kShaders);
    }

    // The image shows its memory as it was read under the watch it has now:
    // nothing of it is behind (TrackImage).
    void NoteStaged(Image& image)
    {
        image.partsKnown = true;
        image.writtenPages.clear();
    }

    bool StageInto(Image& image, const uint8_t* bytes, size_t size, const char** error)
    {
        image.staged++;
        NoteStaged(image);
        if (g.open) return RecordUpload(*g.open, image, bytes, size, error);

        StagingBuffer staging;
        if (!MakeStaging(staging, size)) return Fail("no staging buffer", error);
        std::memcpy(staging.mapped, bytes, size);
        bool ok = false;
        if (VkCommandBuffer cmd = BeginCommands())
        {
            RecordCopy(cmd, image, staging.buffer, 0);
            ok = EndCommands();
        }
        FreeStaging(staging);
        g.uploadsAlone++;
        if (!ok) return Fail("upload submission failed", error);
        return true;
    }

    // An animated replacement's image at its first bind of a frame: the frame
    // the clock has reached staged over the one it shows, when that is
    // another. The image, its id and its view stay; only its bytes change,
    // ahead of the draws of the segment that binds it, as a rewritten
    // texture's do. Every frame was decoded when the image was made, so this
    // is a copy into staging and a copy command, and it is left for the next
    // frame when uploads are not open rather than submitted alone and waited
    // for. An image nothing binds is never looked at.
    void Animate(Image& image)
    {
        if (!g.open) return;
        const std::shared_ptr<const gpu::TextureData> picture =
            gpu::replace::Due(image.skin, image.frame);
        if (!picture || picture->bytes.size() != image.bytes) return;
        StageInto(image, picture->bytes.data(), picture->bytes.size(), nullptr);
    }

    VkSamplerAddressMode AddressMode(gpu::TextureClamp clamp)
    {
        // Mirror-once needs its extension enabled (pipeline.cpp). Without it
        // the nearest mode mirrors and repeats, which is the same picture for
        // coordinates within one texture of the edge.
        const VkSamplerAddressMode mirrorOnce = vk::pipeline::MirrorClampToEdge()
            ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        switch (clamp)
        {
        case gpu::TextureClamp::Repeat:               return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case gpu::TextureClamp::MirroredRepeat:       return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case gpu::TextureClamp::ClampToEdge:          return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case gpu::TextureClamp::MirrorClampToEdge:    return mirrorOnce;
        // "Halfway" clamps at the edge texel's centre rather than its outer edge,
        // which Vulkan has no mode for. Clamping is the near miss.
        case gpu::TextureClamp::ClampToHalfway:       return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case gpu::TextureClamp::MirrorClampToHalfway: return mirrorOnce;
        case gpu::TextureClamp::ClampToBorder:        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case gpu::TextureClamp::MirrorClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }

    VkFilter FilterOf(gpu::TextureFilter filter)
    {
        return filter == gpu::TextureFilter::Point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    }

    // The kind a view was made as, in shader::Translation::textureKinds' terms.
    uint8_t KindOf(const Image& image)
    {
        return image.volume ? 2 : image.layers > 1 ? 1 : 0;
    }

    // An id is an image and, above bit 32, the sampler it is bound with: an
    // index into State::samplerList plus one, or zero for the image's own.
    constexpr uint64_t kImageBits = 0xFFFFFFFFull;
    constexpr uint32_t kSamplerShift = 32;

    // An image's place in the bindless set's array: after the stand-in of
    // each kind (bindings.h), so its id, one-based, plus two.
    uint32_t ElementOf(uint64_t id)
    {
        return uint32_t(id & kImageBits) + vk::bindings::kImageKinds - 1;
    }

    // The bindless set's entry at `element`: the image's view.
    void WriteImage(uint32_t element, Image& image)
    {
        VkDescriptorImageInfo info{ VK_NULL_HANDLE, image.view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = g.bindless;
        write.dstBinding = vk::bindings::kImages;
        write.dstArrayElement = element;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
        image.written = image.view;
        g.descriptorsWritten++;
    }

    // One sampler per distinct state, keyed on the fetch constant's own bits
    // and the lowest level the texture may be sampled at. Its slot in the
    // bindless set is written as it is made; a sampler is never destroyed
    // before the cache is, so the slot holds it for good.
    State::Sampler SamplerFor(uint64_t key)
    {
        auto found = g.samplers.find(key);
        if (found != g.samplers.end()) return found->second;
        const uint32_t slot = uint32_t(g.samplers.size());
        if (slot >= vk::pipeline::Samplers()) return {};

        VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        info.magFilter = FilterOf(gpu::TextureFilter((key >> 0) & 3));
        info.minFilter = FilterOf(gpu::TextureFilter((key >> 2) & 3));
        info.mipmapMode = gpu::TextureFilter((key >> 4) & 3) == gpu::TextureFilter::Linear
                            ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = AddressMode(gpu::TextureClamp((key >> 6) & 7));
        info.addressModeV = AddressMode(gpu::TextureClamp((key >> 9) & 7));
        info.addressModeW = AddressMode(gpu::TextureClamp((key >> 12) & 7));
        // A cube's faces clamp at their edges, each filtered on its own as
        // Direct3D 9 filters them, where the device allows it.
        if ((key >> 24) & 1)
            info.addressModeU = info.addressModeV = info.addressModeW =
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vk::pipeline::NonSeamlessCubes())
            info.flags |= VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT;
        info.borderColor = ((key >> 15) & 3) == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                  : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        // The image holds exactly the levels the fetch constant allows above
        // the lowest, so only that end needs a clamp. `BaseMap` samples the
        // lowest level alone. A clamp of exactly that level would pin the LOD
        // at it, and a LOD that never rises above zero never picks the min
        // filter; a quarter lets it, while every level choice still rounds to
        // the lowest. Follows Xenia (BSD-3).
        info.minLod = float((key >> 17) & 15);
        info.maxLod = gpu::TextureFilter((key >> 4) & 3) == gpu::TextureFilter::BaseMap
                        ? info.minLod + 0.25f : VK_LOD_CLAMP_NONE;
        // 1:1 is no anisotropy; 2:1 and up double each step. Only a sample
        // whose LOD comes from derivatives filters along them, which is the
        // hardware's rule too.
        const uint32_t aniso = (key >> 21) & 7;
        if (aniso >= 2 && aniso <= 5 && vk::pipeline::MaxAnisotropy() > 1.0f)
        {
            info.anisotropyEnable = VK_TRUE;
            info.maxAnisotropy =
                std::min(float(1u << (aniso - 1)), vk::pipeline::MaxAnisotropy());
        }

        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(g.device, &info, nullptr, &sampler) != VK_SUCCESS) return {};
        VkDescriptorImageInfo image{ sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = g.bindless;
        write.dstBinding = vk::bindings::kSamplers;
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
        return g.samplers[key] = { sampler, slot };
    }

    // The sampler's place in an id, making it on first sight; zero when it
    // could not be made.
    uint32_t SamplerIndexFor(uint64_t key)
    {
        auto found = g.samplerIndex.find(key);
        if (found != g.samplerIndex.end()) return found->second;
        const State::Sampler sampler = SamplerFor(key);
        if (!sampler.sampler) return 0;
        g.samplerList.push_back(sampler);
        return g.samplerIndex[key] = uint32_t(g.samplerList.size());
    }

    // `minLevel` is the lowest level the image holds a picture for. Under
    // `BaseMap` it is the base whenever the base was read, whatever the fetch
    // constant's own lowest level.
    uint64_t SamplerKey(const gpu::TextureFetch& fetch, uint32_t minLevel)
    {
        if (fetch.MipFilter() == gpu::TextureFilter::BaseMap && minLevel &&
            gpu::ExtentsOf(fetch).baseBytes)
            minLevel = 0;
        return (uint64_t(fetch.MagFilter()) << 0) | (uint64_t(fetch.MinFilter()) << 2) |
               (uint64_t(fetch.MipFilter()) << 4) | (uint64_t(fetch.ClampX()) << 6) |
               (uint64_t(fetch.ClampY()) << 9) | (uint64_t(fetch.ClampZ()) << 12) |
               (uint64_t(fetch.BorderColour()) << 15) | (uint64_t(minLevel & 15) << 17) |
               (uint64_t(fetch.Anisotropy()) << 21) |
               (uint64_t(fetch.Dimension() == gpu::TextureDimension::Cube) << 24);
    }

    // The stand-in of each kind, at its kind's element of set 1's image array,
    // for a slot with nothing bound and for one bound to a texture of another
    // kind than the module declares: a view of the wrong kind is invalid, and
    // the title does bind a 2D texture where a program fetches a volume, and a
    // 2D one where it fetches a cube.
    bool MakeFallbackOf(Image& out, uint32_t layers, bool volume)
    {
        out.format = VK_FORMAT_R8G8B8A8_UNORM;
        out.width = out.height = 1;
        out.blocksWide = out.blocksHigh = 1;
        out.bytesPerBlock = 4;
        out.layers = layers;
        out.volume = volume;
        out.bytes = 4 * layers;
        if (!MakeImage(out, nullptr)) return false;
#if defined(MW2_ANDROID) || defined(__ANDROID__)
        // White on a phone: what its lighting reads where nothing is bound.
        constexpr uint8_t kFill = 255;
#else
        constexpr uint8_t kFill = 0;
#endif
        const std::vector<uint8_t> texels(4 * layers, kFill);
        if (!StageInto(out, texels.data(), texels.size(), nullptr)) return false;
        const State::Sampler sampler = SamplerFor(0);
        out.sampler = sampler.sampler;
        out.samplerSlot = sampler.slot;
        if (!out.sampler) return false;
        WriteImage(KindOf(out), out);
        return true;
    }

    bool MakeFallback()
    {
        return MakeFallbackOf(g.fallback, 1, false) &&
               MakeFallbackOf(g.fallbackCube, 6, false) &&
               MakeFallbackOf(g.fallback3D, 1, true);
    }

    const Image& FallbackOf(uint8_t kind)
    {
        return kind == 2 ? g.fallback3D : kind == 1 ? g.fallbackCube : g.fallback;
    }

    // A slot of g.images, one-based: zero means nothing. A released image's
    // slot is used again; nothing still holds its id by then (Release).
    uint64_t NewSlot()
    {
        if (!g.freeIds.empty())
        {
            const uint64_t id = g.freeIds.back();
            g.freeIds.pop_back();
            return id;
        }
        // An id also places the image in the bindless set, which has room
        // for so many.
        if (ElementOf(g.images.size() + 1) >= vk::bindings::kImageCount) return 0;
        g.images.emplace_back();
        return uint64_t(g.images.size());
    }

    // `id` from NewSlot.
    void Store(uint64_t id, Image& image)
    {
        image.lastBound = g.frame;
        image.boundSegment = g.segment;
        g.uploads++;
        g.uploadedBytes += image.bytes;
        g.liveBytes += image.bytes;
        Image& stored = g.images[size_t(id - 1)];
        stored = image;
        WriteImage(ElementOf(id), stored);
    }

    Image* Find(uint64_t id)
    {
        id &= kImageBits;
        if (!id || id > g.images.size()) return nullptr;
        return &g.images[size_t(id - 1)];
    }

    // The bindless slot of the sampler an id is bound with.
    uint32_t SamplerSlotOf(uint64_t id, const Image& image)
    {
        const uint32_t index = uint32_t(id >> kSamplerShift) & 0x3FFFFFFFu;
        return index && index <= g.samplerList.size() ? g.samplerList[index - 1].slot
                                                       : image.samplerSlot;
    }

    // Every byte of the texture's memory, four lanes wide to keep the multiplies
    // off one dependency chain. A change to any single word is always seen, since
    // each step is a bijection of the lane's state; sampling instead missed the
    // model-lighting table's 4x4-texel patches.
    uint64_t SignatureOf(const uint8_t* base, size_t bytes)
    {
        constexpr uint64_t kPrime = 0x100000001B3ull;
        uint64_t lane[4] = { 0xCBF29CE484222325ull, 0x84222325CBF29CE4ull,
                             0x9E3779B97F4A7C15ull, 0x7F4A7C159E3779B9ull };
        size_t at = 0;
        for (; at + 32 <= bytes; at += 32)
        {
            uint64_t w[4];
            std::memcpy(w, base + at, sizeof w);
            for (int i = 0; i < 4; i++) lane[i] = (lane[i] ^ w[i]) * kPrime;
        }
        for (; at < bytes; at++) lane[at & 3] = (lane[at & 3] ^ base[at]) * kPrime;
        uint64_t hash = uint64_t(bytes) * 0x9E3779B97F4A7C15ull;
        for (int i = 0; i < 4; i++) hash = (hash ^ lane[i]) * kPrime ^ (lane[i] >> 29);
        return hash;
    }

    uint64_t TimedSignatureOf(const uint8_t* base, size_t bytes)
    {
        if constexpr (!diag::kOn) return SignatureOf(base, bytes);
        const auto started = std::chrono::steady_clock::now();
        const uint64_t signature = SignatureOf(base, bytes);
        g.signNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - started).count());
        return signature;
    }

    // `signature` and `sourceBytes` describe the guest memory the texture was
    // read from, for a texture that came from a fetch constant. `replacement`
    // is the player's picture to upload in place of `data`, which then only
    // says what the texture is (gpu/texture_replace.h).
    uint64_t UploadOne(const gpu::TextureData& data, uint32_t xenosFormat, uint64_t samplerKey,
                       uint32_t swizzle, bool oneDimensional, const char** error,
                       uint64_t signature = 0, size_t sourceBytes = 0, bool linearise = false,
                       const gpu::TextureData* replacement = nullptr)
    {
        if (!g.device) { if (error) *error = "no device"; return vk::textures::kNone; }

        int format = vk::formats::VulkanFormatFor(xenosFormat);
        if (format == VK_FORMAT_UNDEFINED)
        {
            Fail("no Vulkan equivalent for the format", error);
            return vk::textures::kNone;
        }

        gpu::TextureData& decompressed = DecodeScratch();
        const gpu::TextureData* pData = &data;
        if (replacement)
        {
            // RGBA8 whatever the title's format is, and linearised only where
            // the title's own bytes would have been: where its format has an
            // sRGB form. The swizzle below stays the title's, which is why a
            // replacement is held in the host layout of the title's format.
            linearise = linearise && vk::formats::SrgbFormatFor(format) != VK_FORMAT_UNDEFINED;
            format = VK_FORMAT_R8G8B8A8_UNORM;
            pData = replacement;
        }
        else if (DecodeInSoftware(xenosFormat))
        {
            int decompFormat = 0;
            if (!vk::bc::Decompress(data, xenosFormat, decompressed, decompFormat))
            {
                Fail("software BC decompression failed", error);
                LOGE("Texture upload: software BC decompression failed for xenosFormat %u (%ux%u, %zu levels): %s",
                     xenosFormat, data.width, data.height, data.levels.size(),
                     decompressed.error ? decompressed.error : "decoder failure");
                return vk::textures::kNone;
            }
            format = decompFormat;
            pData = &decompressed;
        }

        // The fetch constant asked for the stored value to be linearised. An
        // sRGB image does that in the sampler; the bytes uploaded are the same
        // ones either way, so only the format changes.
        if (linearise)
        {
            const int srgb = vk::formats::SrgbFormatFor(format);
            if (srgb != VK_FORMAT_UNDEFINED) format = srgb;
            else                             g.linearisedRefused++;
        }
        const gpu::TextureFormatInfo info = gpu::TextureFormatOf(xenosFormat);

        // A desynchronised command stream leaves fetch constants that decode to
        // plausible textures of impossible size. Creating one is undefined behaviour
        // rather than an error -- the driver here builds the image and returns
        // garbage -- so the size has to be checked.
        if (pData->width > g.maxDimension || pData->height > g.maxDimension)
        {
            Fail("larger than the device allows", error);
            return vk::textures::kNone;
        }

        Image image;
        image.oneDimensional = oneDimensional && pData->height == 1;
        image.layers = pData->layers;
        image.volume = pData->volume;
        image.sourceBytes = sourceBytes;
        image.signature = signature;
        // What the fetch constant asks for is in guest channels, which are not
        // always the host ones: a guest format with fewer channels than the host
        // format carrying it repeats its last, rather than reading the constants
        // the host format supplies.
        image.swizzle =
            vk::formats::ComposeSwizzle(swizzle, vk::formats::HostSwizzleFor(xenosFormat));
        image.format = VkFormat(format);
        image.width = pData->width;
        image.height = pData->height;
        image.blocksWide = pData->blocksWide;
        image.blocksHigh = pData->blocksHigh;
        image.bytesPerBlock = pData->bytesPerBlock;
        image.blockWidth = pData->expanded ? 1 : info.blockWidth;
        image.blockHeight = pData->expanded ? 1 : info.blockHeight;
        image.bytes = pData->bytes.size();
        image.levels = pData->levels;

        // The sampler and the id before the image: once its upload is
        // recorded, the image may not be destroyed until that has run.
        const State::Sampler sampler = SamplerFor(samplerKey);
        image.sampler = sampler.sampler;
        image.samplerSlot = sampler.slot;
        if (!image.sampler)
        {
            Fail("no sampler", error);
            return vk::textures::kNone;
        }
        const uint64_t id = NewSlot();
        if (!id)
        {
            Fail("no room in the bindless set", error);
            return vk::textures::kNone;
        }
        if (!MakeImage(image, error) ||
            !StageInto(image, pData->bytes.data(), pData->bytes.size(), error))
        {
            DestroyImage(image);
            g.freeIds.push_back(id);
            return vk::textures::kNone;
        }
        Store(id, image);
        return id;
    }
}

namespace
{
    // The threads that read textures again at a segment's end, joined before
    // the device goes (PrepareAll).
    void StopReaders();

    // The one set every draw samples through (bindings.h).
    bool MakeBindlessSet()
    {
        const VkDescriptorPoolSize sizes[2] = {
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, vk::bindings::kImageCount },
            { VK_DESCRIPTOR_TYPE_SAMPLER, vk::bindings::kSamplerCount } };
        VkDescriptorPoolCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        info.maxSets = 1;
        info.poolSizeCount = 2;
        info.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(g.device, &info, nullptr, &g.bindlessPool) != VK_SUCCESS)
            return false;
        VkDescriptorSetLayout layout =
            static_cast<VkDescriptorSetLayout>(vk::pipeline::SetLayout(vk::bindings::kTextureSet));
        VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocate.descriptorPool = g.bindlessPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &layout;
        return layout && vkAllocateDescriptorSets(g.device, &allocate, &g.bindless) == VK_SUCCESS;
    }
}

bool vk::textures::Initialise()
{
    if (g.device) return true;
    if (!vk::pipeline::Ready() && !vk::pipeline::Initialise()) return false;

    g.device = static_cast<VkDevice>(vk::pipeline::Device());
    g.physical = static_cast<VkPhysicalDevice>(vk::pipeline::PhysicalDevice());
    g.queue = static_cast<VkQueue>(vk::pipeline::Queue());
    if (!g.device || !g.queue)
    {
        LOGW("textures: the pipeline device has no queue to upload on");
        g.device = VK_NULL_HANDLE;
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memory);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(g.physical, &properties);
    g.maxDimension = properties.limits.maxImageDimension2D;
    // Half the device's own memory, within reason, unless MW2_TEXTURE_BUDGET_MB
    // says otherwise.
    VkDeviceSize deviceLocal = 0;
    for (uint32_t i = 0; i < g.memory.memoryHeapCount; i++)
        if (g.memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            deviceLocal = std::max(deviceLocal, g.memory.memoryHeaps[i].size);
    g.budget = env::Number("MW2_TEXTURE_BUDGET_MB") << 20;
    if (!g.budget)
    {
#if defined(MW2_ANDROID) || defined(__ANDROID__)
        g.budget = 192ull << 20;
#else
        g.budget = std::clamp<uint64_t>(deviceLocal / 2, 256ull << 20, 2048ull << 20);
#endif
    }

    VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = vk::pipeline::QueueFamily();
    if (vkCreateCommandPool(g.device, &pool, nullptr, &g.commands) != VK_SUCCESS)
    {
        LOGW("textures: no command pool");
        Shutdown();
        return false;
    }

    VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate.commandPool = g.commands;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g.device, &allocate, &g.command) != VK_SUCCESS)
    {
        LOGW("textures: no command buffer");
        Shutdown();
        return false;
    }

    VkFenceCreateInfo fence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(g.device, &fence, nullptr, &g.fence) != VK_SUCCESS)
    {
        LOGW("textures: no fence");
        Shutdown();
        return false;
    }

    for (UploadSlot& slot : g.uploadSlots)
    {
        if (vkCreateCommandPool(g.device, &pool, nullptr, &slot.pool) != VK_SUCCESS)
        {
            LOGW("textures: no upload command buffers");
            Shutdown();
            return false;
        }
    }

    if (!MakeBindlessSet())
    {
        LOGW("textures: no descriptor set for the textures");
        Shutdown();
        return false;
    }

    if (!MakeFallback())
    {
        LOGW("textures: the placeholder image would not upload (%s)", g.lastError.c_str());
        Shutdown();
        return false;
    }

    // One startup snapshot for diagnosing texture corruption on drivers whose
    // compressed-texture support differs from the renderer's expected path.
    const bool bcHardware = vk::pipeline::TextureCompressionBC();
    LOGI("textures: uploading on %s; BC hardware sampling=%s; "
         "MW2_FORCE_BC_DECOMPRESS=%s; texture budget=%llu MiB; max 2D=%u",
         vk::pipeline::DeviceName(), bcHardware ? "yes" : "no",
         env::Flag("MW2_FORCE_BC_DECOMPRESS") ? "set" : "unset",
         static_cast<unsigned long long>(g.budget >> 20), g.maxDimension);
    return true;
}

void vk::textures::Shutdown()
{
    if (!g.device) return;
    StopReaders();
    vkDeviceWaitIdle(g.device);
    for (Image& image : g.doomed) DestroyImage(image);
    g.doomed.clear();
    for (Image& image : g.images) DestroyImage(image);
    g.images.clear();
    g.freeIds.clear();
    g.byFetch.clear();
    g.fetchGeneration++;
    g.byContent.clear();
    g.samplerList.clear();
    g.samplerIndex.clear();
    DestroyImage(g.fallback);
    DestroyImage(g.fallbackCube);
    DestroyImage(g.fallback3D);
    for (auto& [key, sampler] : g.samplers) vkDestroySampler(g.device, sampler.sampler, nullptr);
    g.samplers.clear();
    if (g.bindlessPool) vkDestroyDescriptorPool(g.device, g.bindlessPool, nullptr);
    g.bindlessPool = VK_NULL_HANDLE;
    g.bindless = VK_NULL_HANDLE;
    vk::imagememory::Shutdown(g.device);
    g.open = nullptr;
    for (UploadSlot& slot : g.uploadSlots)
    {
        FreeStaging(slot.staging);
        for (StagingBuffer& extra : slot.overflow) FreeStaging(extra);
        if (slot.pool) vkDestroyCommandPool(g.device, slot.pool, nullptr);
        slot = UploadSlot{};
    }
    if (g.fence) vkDestroyFence(g.device, g.fence, nullptr);
    if (g.commands) vkDestroyCommandPool(g.device, g.commands, nullptr);
    g.fence = VK_NULL_HANDLE;
    g.commands = VK_NULL_HANDLE;
    g.command = VK_NULL_HANDLE;
    g.device = VK_NULL_HANDLE;
}

bool vk::textures::Ready() { return g.device != VK_NULL_HANDLE; }

namespace
{
    // The host address of a texture's first byte, from the physical address its
    // fetch constant holds.
    const uint8_t* HostAddress(uint32_t physicalAddress)
    {
        if (!physicalAddress) return nullptr;
        const uint32_t va = kernel::FromPhysical(physicalAddress);
        return va ? guest::Base() + va : nullptr;
    }

    // Both ranges a texture was read from, as one signature: a rewrite of the
    // chain changes what is sampled as surely as one of the base. By host
    // address, and untimed touching nothing else: what a thread other than
    // the consumer may call.
    uint64_t SignRanges(const uint8_t* base, size_t baseBytes, const uint8_t* mips,
                        size_t mipBytes, bool timed = false)
    {
        auto sign = [&](const uint8_t* at, size_t bytes) {
            return timed ? TimedSignatureOf(at, bytes) : SignatureOf(at, bytes);
        };
        uint64_t signature = 0;
        if (base && baseBytes) signature = sign(base, baseBytes);
        if (mips && mipBytes)
            signature = (signature ^ sign(mips, mipBytes)) * 0x100000001B3ull + 1;
        return signature;
    }

    uint64_t SignSource(uint32_t base, size_t baseBytes, uint32_t mips, size_t mipBytes,
                        bool timed = false)
    {
        return SignRanges(baseBytes ? HostAddress(base) : nullptr, baseBytes,
                          mipBytes ? HostAddress(mips) : nullptr, mipBytes, timed);
    }

    uint64_t SignImage(const Image& image, bool timed = false)
    {
        return SignSource(image.address, image.sourceBytes, image.mipAddress, image.mipBytes,
                          timed);
    }

    // The write watch on a texture's memory, taken before the memory is read:
    // a write after this point shows, one before it is in what is read. Zero
    // when any of it cannot be watched -- outside the physical bank, or pages
    // the title keeps rewriting -- which leaves the texture to its signature.
    uint64_t TrackSource(uint32_t base, size_t baseBytes, uint32_t mips, size_t mipBytes)
    {
        if (!gpu::watch::Enabled() || (!baseBytes && !mipBytes)) return 0;
        uint64_t stamp = 0;
        for (const auto& [address, bytes] : { std::pair{ base, baseBytes }, std::pair{ mips, mipBytes } })
        {
            if (!bytes) continue;
            if (kernel::FromPhysical(address) != guest::kPhysicalBase + address) return 0;
            const uint64_t one = gpu::watch::Track(address, uint32_t(bytes),
                                                   vk::renderer::CompletedSubmission, g.frame);
            if (!one) return 0;
            stamp = std::max(stamp, one);
        }
        return stamp;
    }

    // A texture the title rewrites a little of at a time is read again in
    // parts: the title's own bytes in one level of one-texel blocks, large
    // enough for a part to be much less than the whole. MW2's model lighting
    // volume is the one that matters: 2 MB, a few of its pages written every
    // frame, and reading, swapping and staging all of it at the end of a
    // segment of every frame was a sixth of the ring consumer's time on the
    // AYN Thor and half a millisecond of the GPU's.
    constexpr size_t kPartsLeast = 16 * gpu::watch::kPageBytes;
    constexpr uint32_t kMostWrittenPages = 64;
    bool ReadInParts(const Image& image)
    {
        return image.sourceBytes >= kPartsLeast && !image.mipBytes && image.levels.size() == 1 &&
               !image.skin && image.blockWidth == 1 && image.blockHeight == 1 &&
               !DecodeInSoftware(image.xenosFormat);
    }

    // Watched again, as TrackSource; and for a texture read in parts, the
    // pages written under the stamp this one replaces are added to those the
    // image is behind on. Every watch of an image's memory after its first
    // read comes through here, so the list is whole from one staging to the
    // next, or `partsKnown` says it is not.
    uint64_t TrackImage(Image& image)
    {
        const uint64_t since = image.watchStamp;
        const uint64_t stamp =
            TrackSource(image.address, image.sourceBytes, image.mipAddress, image.mipBytes);
        if (!image.partsKnown || !ReadInParts(image)) return stamp;
        uint32_t pages[kMostWrittenPages];
        const uint32_t count = since && stamp
            ? gpu::watch::WrittenSince(image.address, uint32_t(image.sourceBytes), since, pages,
                                       kMostWrittenPages)
            : UINT32_MAX;
        if (count == UINT32_MAX) { image.partsKnown = false; return stamp; }
        for (uint32_t i = 0; i < count; i++)
            if (std::find(image.writtenPages.begin(), image.writtenPages.end(), pages[i]) ==
                image.writtenPages.end())
                image.writtenPages.push_back(pages[i]);
        if (image.writtenPages.size() > kMostWrittenPages) image.partsKnown = false;
        return stamp;
    }

    // What to read of a rewritten texture in place of all of it: the boxes of
    // the pages written since it was staged, when they are known, uploads are
    // open to stage them into and they come to under a quarter of it. Empty
    // for a texture to read whole.
    //
    // With replacements on, what is read has to be whole to be looked up
    // among them, by its bytes -- until the title has rewritten the texture
    // kOwnBytes times: a streamed picture is whole after a few writes and a
    // replacement is found for it then, and what goes on being rewritten is
    // the title's own data, which no picture in a pack is. Modern Warfare
    // 3's multiplayer has them on for its camouflage packs, and read its
    // model lighting volume whole for it at a segment's end of every frame:
    // 2 MB where two of its 512 pages had changed, 5.6 GB in 85 s of a
    // Seatown match against 0.7 GB for every other texture rewritten.
    constexpr uint32_t kOwnBytes = 64;
    std::vector<gpu::TextureBox> BoxesToRead(Image& image)
    {
        // MW2_TEXTURE_PARTS=0 reads every rewritten texture whole, as before:
        // what reading in parts saves, and a picture to compare with.
        static const bool parts = !env::Text("MW2_TEXTURE_PARTS") || env::Flag("MW2_TEXTURE_PARTS");
        if (!parts || !g.open || !image.partsKnown || image.writtenPages.empty() ||
            !ReadInParts(image) || (gpu::replace::On() && image.staged < kOwnBytes))
            return {};
        if (!image.boxesAsked)
        {
            image.boxesAsked = true;
            gpu::TextureFetch fetch;
            std::memcpy(fetch.d, image.fetchWords, sizeof fetch.d);
            image.pageBoxes = gpu::TextureBoxesByPage(fetch, gpu::watch::kPageBytes);
        }
        std::vector<gpu::TextureBox> boxes;
        size_t blocks = 0;
        const uint32_t slices = image.volume ? image.layers : 1;
        for (const uint32_t page : image.writtenPages)
        {
            if (page >= image.pageBoxes.size()) return {};
            const gpu::TextureBox& box = image.pageBoxes[page];
            // A page of the texture's memory that holds none of its blocks.
            if (box.Empty()) continue;
            if (box.x + box.w > image.width || box.y + box.h > image.height || box.z + box.d > slices)
                return {};
            boxes.push_back(box);
            blocks += box.Blocks();
        }
        if (blocks * 4 > size_t(image.width) * image.height * slices) return {};
        return boxes;
    }

    bool Unchanged(const Image& image)
    {
        return image.watchStamp &&
               (!image.sourceBytes ||
                gpu::watch::Unwritten(image.address, uint32_t(image.sourceBytes), image.watchStamp)) &&
               (!image.mipBytes ||
                gpu::watch::Unwritten(image.mipAddress, uint32_t(image.mipBytes), image.watchStamp));
    }

    // How much of a block reads as never written, in percent. Counting zero
    // bytes does not do it: a dark texture is full of them and a compressed
    // block of black is entirely zero, quite correctly. Memory nothing has
    // written is zero in long unbroken stretches, so what is counted is spans of
    // sixty-four bytes that are zero all through.
    uint8_t EmptyPercent(const uint8_t* base, size_t bytes)
    {
        if (!base || bytes < 64) return 0;
        size_t empty = 0, spans = 0;
        for (size_t at = 0; at + 64 <= bytes; at += 64, spans++)
        {
            size_t i = 0;
            while (i < 64 && !base[at + i]) i++;
            if (i == 64) empty++;
        }
        return spans ? uint8_t(empty * 100 / spans) : 0;
    }

    // Whether what was just read into this image looks like a block nothing has
    // written: `empty` is EmptyPercent of `data`. Measured on the decoded
    // texture, not the padded source extent: the padding of a small texture is
    // legitimately untouched. An image that becomes empty is queued to be read
    // again at the end of its segment.
    void NoteEmptiness(Image& image, const gpu::TextureData& data, uint8_t empty)
    {
        if ((!image.address && !image.mipAddress) || data.bytes.empty()) return;
        if (empty <= 25) { image.fromAnEmptyBlock = false; return; }
        if (empty <= 60) return;
        if (!image.fromAnEmptyBlock && g.emptyThisSegment.size() < 64)
            g.emptyThisSegment.push_back(uint64_t(&image - g.images.data()) + 1);
        image.fromAnEmptyBlock = true;
        if (!image.emptySince) image.emptySince = g.frame ? g.frame : 1;
    }

    // Every name the cache knows the image by, forgotten: the next bind of
    // any of its fetch constants makes an image anew (Upload).
    void Unname(uint64_t id, const Image& image)
    {
        g.epoch++;
        g.fetchGeneration++;
        for (const auto& name : image.names)
            if (auto entry = g.byFetch.find(name);
                entry != g.byFetch.end() && (entry->second & kImageBits) == id)
                g.byFetch.erase(entry);
        if (auto entry = g.byContent.find(image.content);
            entry != g.byContent.end() && entry->second == id)
            g.byContent.erase(entry);
    }

    // Unnamed, but kept as it is: draws already recorded name it by its id,
    // and neither it nor its entry in the bindless set may change under
    // them. SweepImages lets it go once it has been unbound long enough.
    void Orphan(uint64_t id)
    {
        Image& image = g.images[size_t(id - 1)];
        if (image.orphaned) return;
        Unname(id, image);
        image.names.clear();
        image.orphaned = true;
    }

    // A texture read again as its memory holds it now, through the fetch
    // constant it was uploaded from, in two halves. Prepare is the half that
    // is arithmetic on guest memory -- the signature, the read and its byte
    // swap, the software decode, the measure of how empty it came out -- and
    // touches nothing but the job, so the jobs of a segment's end are prepared
    // on several threads at once (PrepareAll). Stage is the copy into staging
    // and the commands, which stay with the consumer and in the order the
    // textures were listed.
    struct Reread
    {
        Reread(uint64_t named, Image& from) : id(named), image(&from)
        {
            std::memcpy(fetch.d, from.fetchWords, sizeof fetch.d);
            base = HostAddress(from.address);
            mips = HostAddress(from.mipAddress);
            baseBytes = from.sourceBytes;
            mipBytes = from.mipBytes;
            xenosFormat = from.xenosFormat;
            format = from.format;
            bytes = from.bytes;
            bytesPerBlock = from.bytesPerBlock;
            decode = DecodeInSoftware(from.xenosFormat);
            skin = wanted = from.skin;
        }

        uint64_t id;
        Image* image;   // the consumer's alone: Prepare works from the copies below
        gpu::TextureFetch fetch;
        const uint8_t* base = nullptr;
        const uint8_t* mips = nullptr;
        size_t baseBytes = 0, mipBytes = 0;
        uint32_t xenosFormat = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkDeviceSize bytes = 0;
        uint32_t bytesPerBlock = 0;
        bool decode = false;                   // through the software decoder
        gpu::TextureData* scratch = nullptr;   // where that decodes to (PrepareAll)
        // Signed first, and read only if that is not `held`, the signature of
        // what the image holds.
        bool signFirst = false;
        uint64_t held = 0;
        bool measure = false;   // `empty` is wanted
        // A texture that came up empty: measured, wanted only if it no longer
        // is, and signed once read, as what the image then holds.
        bool refill = false;
        // The replacement the image holds, by its key, and the one that what
        // Prepare read asks for; zero is the title's own bytes. The same
        // unless Prepare read something that says otherwise.
        uint64_t skin = 0, wanted = 0;

        // What Prepare leaves.
        gpu::TextureData data;                         // as read
        // What to stage, null when nothing is: `data` or `*scratch`, so the
        // job stays where it is once prepared.
        const gpu::TextureData* payload = nullptr;
        uint8_t empty = 0;                             // EmptyPercent of `data`
        uint64_t signature = 0, signNanos = 0;
        // Read in parts (BoxesToRead): the boxes, and their blocks one box
        // after another in `boxBytes`, each from a multiple of sixteen bytes.
        // Nothing else is read then: no `data`, no payload, no measure.
        std::vector<gpu::TextureBox> boxes;
        std::vector<size_t> boxOffsets;
        std::vector<uint8_t> boxBytes;
    };

    // What the image is staged from: the texture as read, or its software
    // decode. Null when that is not what the image was made to hold.
    const gpu::TextureData* PayloadOf(const Reread& job)
    {
        const gpu::TextureData* pData = &job.data;
        if (job.decode)
        {
            gpu::TextureData& decompressed = *job.scratch;
            int decompFormat = 0;
            if (!vk::bc::Decompress(job.data, job.xenosFormat, decompressed, decompFormat))
            {
                LOGE("Texture restage: software BC decompression failed for xenosFormat %u (%ux%u, %zu levels): %s",
                     job.xenosFormat, job.data.width, job.data.height, job.data.levels.size(),
                     decompressed.error ? decompressed.error : "decoder failure");
                return nullptr;
            }
            if (job.format != VkFormat(decompFormat) &&
                job.format != VkFormat(vk::formats::SrgbFormatFor(decompFormat)))
            {
                LOGE("Texture restage: format mismatch for xenosFormat %u (got %d, image has %d)",
                     job.xenosFormat, decompFormat, int(job.format));
                return nullptr;
            }
            pData = &decompressed;
        }

        if (!pData->ok)
        {
            LOGE("Texture restage: payload data is invalid for xenosFormat %u", job.xenosFormat);
            return nullptr;
        }

        if (pData->bytes.size() != job.bytes)
        {
            LOGE("Texture restage: payload size mismatch for xenosFormat %u (got %zu bytes, image expects %llu bytes)",
                 job.xenosFormat, pData->bytes.size(), static_cast<unsigned long long>(job.bytes));
            return nullptr;
        }
        return pData;
    }

    // The half of Prepare for a job read in parts: each box as the whole
    // read would have left it.
    void ReadBoxes(Reread& job)
    {
        size_t bytes = 0;
        for (const gpu::TextureBox& box : job.boxes)
        {
            bytes = (bytes + 15) & ~size_t(15);
            job.boxOffsets.push_back(bytes);
            bytes += box.Blocks() * job.bytesPerBlock;
        }
        job.boxBytes.resize(bytes);
        for (size_t i = 0; i < job.boxes.size(); i++)
            gpu::ReadTextureBox(job.fetch, job.base, job.boxes[i],
                                job.boxBytes.data() + job.boxOffsets[i]);
    }

    // On any thread: nothing here reads or writes what another job, the cache
    // or Vulkan holds. The replacements are behind a lock of their own
    // (gpu/texture_replace.h).
    void Prepare(Reread& job)
    {
        if (job.signFirst)
        {
            if constexpr (diag::kOn)
            {
                const auto started = std::chrono::steady_clock::now();
                job.signature = SignRanges(job.base, job.baseBytes, job.mips, job.mipBytes);
                job.signNanos = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - started).count());
            }
            else job.signature = SignRanges(job.base, job.baseBytes, job.mips, job.mipBytes);
            if (job.signature == job.held) return;
        }
        if (!job.boxes.empty()) { ReadBoxes(job); return; }
        job.data = gpu::ReadTexture(job.fetch, job.base, job.mips);
        if (!job.data.ok) return;
        if (job.measure || job.refill)
            job.empty = EmptyPercent(job.data.bytes.data(), job.data.bytes.size());
        if (job.refill && job.empty > 25) return;
        if (gpu::replace::On())
        {
            const uint64_t key = gpu::replace::KeyOf(job.data, job.xenosFormat);
            gpu::replace::Dump(key, job.data, job.fetch);
            // Decoded here -- on a reader, where the batch has one -- for
            // the image the next bind makes of it (Restage).
            job.wanted = key == job.skin ? key
                       : gpu::replace::Load(key, job.data, job.fetch, SampledAsGamma(job.fetch))
                             ? key : 0;
        }
        // A replacement's image is not in the shape of what was read, and
        // takes none of it.
        if (!job.skin) job.payload = PayloadOf(job);
        if (job.refill && (job.payload || job.skin))
            job.signature = SignRanges(job.base, job.baseBytes, job.mips, job.mipBytes);
    }

    // Staged over the old contents, if the read came out in their shape.
    // False when it could not be.
    bool Stage(const Reread& job)
    {
        return job.payload && StageInto(*job.image, job.payload->bytes.data(),
                                        job.payload->bytes.size(), nullptr);
    }

    // The boxes of a job read in parts, staged over the image's texels there.
    // Asked for only with uploads open (BoxesToRead).
    bool StageBoxes(const Reread& job)
    {
        Image& image = *job.image;
        if (!g.open || job.boxBytes.empty()) return false;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint8_t* staged = StagingFor(*g.open, job.boxBytes.size(), buffer, offset, nullptr);
        if (!staged) return false;
        std::memcpy(staged, job.boxBytes.data(), job.boxBytes.size());
        RecordBoxCopies(g.open->commands[g.open->segment], image, buffer, offset, job.boxes,
                        job.boxOffsets);
        image.staged++;
        NoteStaged(image);
        g.uploadsBatched++;
        g.stagedInParts++;
        g.partBytes += job.boxBytes.size();
        g.wholeBytes += image.bytes;
        return true;
    }

    // A re-read staged where the image can take it, and the image left
    // showing what the read asks for, or let go so that the next bind makes
    // one that does. An image is made either as the title's texture or as a
    // replacement, of a size and format of its own (gpu/texture_replace.h),
    // and cannot become the other. So when the bytes read ask for the other
    // -- a streamed texture arriving that the player has a replacement for,
    // the title putting another texture where a replaced one was -- the
    // image is orphaned; the title's own bytes are staged first all the
    // same, for the draws of this segment that already name it. False when
    // the image shows nothing new.
    bool Restage(const Reread& job)
    {
        if (!job.boxes.empty()) return StageBoxes(job);
        // Still the replacement it holds: nothing of what was read is shown.
        if (job.skin && job.wanted == job.skin) return job.data.ok;
        const bool staged = !job.skin && Stage(job);
        if (job.wanted != job.skin) Orphan(job.id);
        return staged;
    }

    // MW2_TEXTURE_THREADS=<n>: how many threads besides the consumer prepare
    // the re-reads of a segment's end; 0 keeps every one of them on the
    // consumer, one after another. Three, because a texture the title's image
    // pool rewrote is read, swapped and decoded again at the end of every
    // segment that binds it, a segment often has several, and on a phone that
    // work made the consumer late for a frame a dozen times a minute.
    size_t ReaderThreads()
    {
        static const size_t threads =
            size_t(std::min<uint64_t>(env::Number("MW2_TEXTURE_THREADS", 3), 8));
        return threads;
    }

    // Fork and join: the consumer hands over a batch of jobs, takes its share
    // of them and does not go on until every one is done, so guest memory is
    // read at the same point of the command stream as when it read them all
    // itself.
    struct Readers
    {
        std::mutex lock;
        std::condition_variable work, done;
        std::vector<std::thread> threads;
        Reread* jobs = nullptr;                    // the batch, while there is one
        size_t count = 0, next = 0, finished = 0;
        bool stopping = false;
        // What the batch's jobs after the first decode into, kept from one
        // batch to the next as DecodeScratch is; and that a smaller budget
        // has asked for them back since.
        std::vector<gpu::TextureData> scratch;
        std::atomic<bool> trim{ false };
    };
    Readers readers;

    // The batch's jobs nobody has taken yet, one at a time. Called and left
    // with the lock held.
    void TakeJobs(std::unique_lock<std::mutex>& lock)
    {
        while (readers.next < readers.count)
        {
            Reread& job = readers.jobs[readers.next++];
            lock.unlock();
            Prepare(job);
            lock.lock();
            readers.finished++;
        }
    }

    void RunReader()
    {
        crash::RegisterThread("texture reader");
        // With the recorder and the title's own threads, off the little cores:
        // the consumer is waiting for these.
        platform::SetThreadAffinity(platform::ThreadAffinityRole::PerformanceCore);
        std::unique_lock lock(readers.lock);
        for (;;)
        {
            readers.work.wait(lock, [] { return readers.next < readers.count || readers.stopping; });
            if (readers.stopping) return;
            TakeJobs(lock);
            if (readers.finished == readers.count) readers.done.notify_one();
        }
    }

    void StopReaders()
    {
        {
            std::lock_guard lock(readers.lock);
            readers.stopping = true;
        }
        readers.work.notify_all();
        for (std::thread& thread : readers.threads) thread.join();
        readers.threads.clear();
        readers.scratch.clear();
        readers.stopping = false;
    }

    // Prepares `count` jobs, at most one more than ReaderThreads, and returns
    // when all are. One job is the consumer's own, with nothing handed over.
    void PrepareAll(Reread* jobs, size_t count)
    {
        jobs[0].scratch = &DecodeScratch();
        if (count == 1) { Prepare(jobs[0]); return; }

        // Started by the first batch, so a run that never has one has no
        // threads either.
        if (readers.threads.empty())
        {
            readers.scratch.resize(ReaderThreads());
            for (size_t i = 0; i < ReaderThreads(); i++) readers.threads.emplace_back(RunReader);
        }
        // No reader is at work between batches, so the buffers are free to go.
        if (readers.trim.exchange(false))
            for (gpu::TextureData& scratch : readers.scratch) ReleaseScratch(scratch);
        for (size_t i = 1; i < count; i++) jobs[i].scratch = &readers.scratch[i - 1];

        std::unique_lock lock(readers.lock);
        readers.jobs = jobs;
        readers.count = count;
        readers.next = readers.finished = 0;
        // A reader for each job but the one the consumer starts on.
        for (size_t i = 1; i < count; i++) readers.work.notify_one();
        TakeJobs(lock);
        readers.done.wait(lock, [] { return readers.finished == readers.count; });
        readers.jobs = nullptr;
        readers.count = readers.next = readers.finished = 0;
    }

    // The re-reads of a segment's end. `gather` is asked about each listed
    // texture in turn and adds a job to the batch for one that is to be read;
    // a batch is prepared together once it has a job for the consumer and
    // each reader, and `finish` then called for its jobs in the order they
    // were listed. What `gather` does for one texture never depends on what
    // `finish` did for another, so the outcome is the one of asking, reading
    // and finishing each in turn -- which is what MW2_TEXTURE_THREADS=0 does.
    template <typename Gather, typename Finish>
    void RereadEach(const std::vector<uint64_t>& ids, Gather gather, Finish finish)
    {
        if (ids.empty()) return;
        const size_t most = 1 + ReaderThreads();
        std::vector<Reread> batch;
        batch.reserve(most);
        const auto flush = [&] {
            if (batch.empty()) return;
            PrepareAll(batch.data(), batch.size());
            for (Reread& job : batch) finish(job);
            batch.clear();
        };
        for (const uint64_t id : ids)
        {
            // Listed twice, it is asked about the second time as the first
            // left it.
            if (std::any_of(batch.begin(), batch.end(),
                            [&](const Reread& job) { return job.id == id; }))
                flush();
            Image* image = Find(id);
            if (!image || !image->image || image->borrowed || image->orphaned) continue;
            gather(id, *image, batch);
            if (batch.size() == most) flush();
        }
        flush();
    }

    // The fetch constant is unchanged when only the bytes are, so the cache would
    // otherwise hand back the upload taken the first time the texture was bound.
    void RefreshIfChanged(uint64_t id)
    {
        Image* image = Find(id);
        if (!image || !image->image || image->borrowed ||
            (!image->sourceBytes && !image->mipBytes))
            return;
        // A texture the title rewrites is read where a frame first binds it and
        // left alone until the next frame does. The commands are read here a
        // frame behind the title, which by then writes the next frame's bytes
        // into the same texture; read later, at the end of the segment, a
        // texture was found half made or already the next frame's:
        // - Black Ops III's model lighting table (a 512x256x4 volume, each
        //   model a patch of it, as Modern Warfare 2's): a teleporter's door,
        //   its pad, the held weapon or a zombie's arm lit as something else or
        //   not at all for one frame, four or five times in 46 s of standing
        //   still in The Giant on a Retroid Pocket Flip 2, and never in runs
        //   with the table read once;
        // - Advanced Warfare's drop pod in Induction: the screens over its
        //   window gone and the window washed white for one frame, on Windows
        //   twice in each of two 150 s runs at 60 frames a second and 14 times
        //   in one at 250, in one run of `aw-campaign` in two on a Retroid
        //   Pocket Flip 2, and in none of six runs read once (docs/textures.md).
        // MW2_TEXTURES_AT_BIND=0 reads them again at the end of each segment
        // that binds them, as before.
        static const bool atBind = env::Text("MW2_TEXTURES_AT_BIND") ? env::Flag("MW2_TEXTURES_AT_BIND") : true;
        const auto queueForSegment = [&] {
            if (atBind) return;
            if (image->queuedSegment == g.segment || g.rewrittenThisSegment.size() >= 256) return;
            image->queuedSegment = g.segment;
            g.rewrittenThisSegment.push_back(id);
        };
        if (image->rewritten) queueForSegment();
        if (image->checkedFrame == g.frame) return;

        if (Unchanged(*image))
        {
            image->checkedFrame = g.frame;
            g.unchangedByWatch++;
            return;
        }
        // Watched, and written since: due now, whatever the schedule says.
        if (!image->watchStamp && g.frame - image->checkedFrame < image->signPeriod) return;
        image->checkedFrame = g.frame;
        g.resigned++;
        stutters::Timed timed(stutters::kRefreshedTextures);
        report::Timed spent(report::kTextures);

        image->watchStamp = TrackImage(*image);
        const uint64_t now = SignImage(*image, true);
        if (now == image->signature)
        {
            // An image that came up empty is checked every frame until it holds a
            // picture: "the bytes have not moved" is no reason to look less often
            // when the point is to catch the moment they do.
            if (!image->rewritten && !image->fromAnEmptyBlock &&
                image->signPeriod < kSignPeriodMax)
                image->signPeriod *= 2;
            // Written back to what the image holds: nothing is owed any more.
            image->restageDue = false;
            return;
        }
        image->rewritten = true;
        image->signPeriod = 1;
        queueForSegment();

        // On the segment's list with its uploads open, the end of the segment
        // signs it again and stages it over whatever is staged here, in the
        // same command buffer, ahead of the same draws -- and a texture the
        // title is in the middle of rewriting has usually moved again by then,
        // which made this read, its decode and its copy work nothing ever
        // sampled (the read alone was 5.5% of the ring consumer). So it is
        // left to that point, which reads it once.
        if (g.open && image->queuedSegment == g.segment)
        {
            image->restageDue = true;
            image->dueSignature = now;
            return;
        }
        image->restageDue = false;
        Reread job(id, *image);
        job.measure = true;
        PrepareAll(&job, 1);
        if (!Restage(job)) return;
        image->signature = now;
        g.refreshed++;
        NoteEmptiness(*image, job.data, job.empty);
    }
}

uint64_t vk::textures::FrameNumber() { return g.frame; }

uint64_t vk::textures::LiveBytes() { return g.liveBytes; }
uint64_t vk::textures::Budget() { return g.budget; }

// A budget the system asked for. The eviction pass is left to the next frame
// -- it waits for the device, and the caller is whichever thread Android
// chose to warn us on.
void vk::textures::SetBudget(uint64_t bytes)
{
    const uint64_t floor = 48ull << 20;   // below this nothing would stay cached
    // A smaller budget is a phone asking for memory back, so the decoder's own
    // buffer goes with it, and the readers' at their next batch; the next
    // restage allocates it again.
    if (bytes < g.budget)
    {
        ReleaseScratch(DecodeScratch());
        readers.trim.store(true);
    }
    g.budget = std::max(bytes, floor);
}

void vk::textures::MemoryWritten(uint32_t physicalAddress, uint32_t size)
{
    // A linear walk: a few thousand images, a couple of copies a frame.
    auto overlaps = [&](uint32_t address, size_t bytes) {
        return bytes && address < physicalAddress + size && address + bytes > physicalAddress;
    };
    g.epoch++;
    for (Image& image : g.images)
    {
        if (!image.image || image.borrowed) continue;
        if (!overlaps(image.address, image.sourceBytes) &&
            !overlaps(image.mipAddress, image.mipBytes))
            continue;
        image.checkedFrame = ~uint64_t(0);
        image.signPeriod = 1;
        g.invalidatedByWrites++;
    }
}

namespace
{
    // How long an image goes unbound before it may be let go: ten seconds of
    // frames, far past any submission still running or any command the
    // recorder thread has yet to make -- which is what lets its id, and its
    // entry in the bindless set, be given to another image. Then how often
    // the cache is looked over: once in this many frames, the images a slice
    // a frame. A phone lets go sooner and looks more often.
#if defined(MW2_ANDROID) || defined(__ANDROID__)
    constexpr uint64_t kUnusedFrames = 120;
    constexpr uint64_t kSweepFrames = 32;
#else
    constexpr uint64_t kUnusedFrames = 600;
    constexpr uint64_t kSweepFrames = 64;
#endif

    // Whether an image can be let go: one of the cache's own, and unbound for
    // that long.
    bool Idle(const Image& image)
    {
        return image.image && !image.borrowed && !image.pinned &&
               g.frame - image.lastBound >= kUnusedFrames;
    }

    // The image stops existing for the cache now; its Vulkan objects are
    // destroyed over the next frames (DestroyDoomed).
    void Release(uint64_t id)
    {
        Image& image = g.images[size_t(id - 1)];
        Unname(id, image);
        g.liveBytes -= image.bytes;
        g.doomed.push_back(std::move(image));
        image = Image{};
        g.freeIds.push_back(id);
    }

    // A few a frame: destroying one is cheap now that its memory is a range of
    // a block, but a sweep can let thousands go at once.
    void DestroyDoomed()
    {
        constexpr auto kLongest = std::chrono::microseconds(500);
        const auto from = std::chrono::steady_clock::now();
        while (!g.doomed.empty())
        {
            DestroyImage(g.doomed.front());
            g.doomed.pop_front();
            if (std::chrono::steady_clock::now() - from >= kLongest) break;
        }
    }

    // The cache holds copies of guest memory, and the console holds none: when
    // the title puts another texture where one was, the old one is gone. So an
    // idle image whose memory has been written since it was read is released.
    // Bound again it would have to be read again anyway, and a level that
    // streams keeps reusing the same memory -- forty thousand such copies, two
    // gigabytes, in seven minutes of one. The write watch says which may have
    // been, and its signature whether it was; an image the watch cannot cover
    // is left to the budget.
    void SweepImages()
    {
        const uint64_t phase = g.frame % kSweepFrames;
        const size_t count = g.images.size();
        const size_t from = size_t(count * phase / kSweepFrames);
        const size_t to = size_t(count * (phase + 1) / kSweepFrames);
        for (size_t i = from; i < to; i++)
        {
            Image& image = g.images[i];
            if (!Idle(image)) continue;
            // Nothing names it (Orphan), and no draw has for long enough.
            if (image.orphaned) { Release(uint64_t(i + 1)); continue; }
            if (!image.watchStamp || Unchanged(image)) continue;
            // The watch answers for whole pages, and a neighbour's write is
            // not this texture's: the bytes decide.
            const uint64_t stamp = TrackImage(image);
            if (SignImage(image) == image.signature)
            {
                image.watchStamp = stamp;
                g.staleNotReally++;
                continue;
            }
            g.stale++;
            g.staleBytes += image.bytes;
            Release(uint64_t(i + 1));
        }
    }

    // Past the budget, the idle images bound longest ago go, down to three
    // quarters of it.
    void Evict()
    {
        const uint64_t target = g.budget / 4 * 3;
        uint64_t count = 0, bytes = 0;

        auto evictOldest = [&](uint64_t graceFrames) {
            std::vector<std::pair<uint64_t, uint64_t>> candidates;
            for (size_t i = 0; i < g.images.size(); i++)
            {
                const Image& img = g.images[i];
                if (img.image && !img.borrowed && !img.pinned &&
                    g.frame - img.lastBound >= graceFrames)
                {
                    candidates.push_back({ img.lastBound, uint64_t(i + 1) });
                }
            }
            if (candidates.empty()) return;
            std::sort(candidates.begin(), candidates.end());
            for (const auto& [lastBound, id] : candidates)
            {
                if (g.liveBytes <= target) break;
                bytes += g.images[size_t(id - 1)].bytes;
                count++;
                Release(id);
            }
        };

#if defined(MW2_ANDROID) || defined(__ANDROID__)
        // First pass: normal idle age
        evictOldest(kUnusedFrames + kSweepFrames);

        // Emergency pass: if still over budget (e.g. streaming rush), the
        // images not bound in recent frames go too. "Recent" has to reach
        // past every frame slot whose recorded commands can still name an
        // image -- a slot is reused only once the GPU is done with it, so
        // that is one frame a slot at most -- and then some.
        constexpr uint64_t kEmergencyFrames = 8 * vk::textures::kUploadSlots;
        static_assert(kEmergencyFrames > vk::textures::kUploadSlots);
        if (g.liveBytes > target)
        {
            evictOldest(kEmergencyFrames);
        }
#else
        // The idle ones only (Idle): an id let go sooner could be given to
        // another image while a command yet to be made still names it.
        evictOldest(kUnusedFrames);
#endif

        g.evicted += count;
        g.evictedBytes += bytes;
        if (g.evictionPasses++ < 4)
            LOGI("textures: over the %llu MB budget; evicted %llu (%llu MB)",
                 (unsigned long long)(g.budget >> 20), (unsigned long long)count,
                 (unsigned long long)(bytes >> 20));
    }
}

void vk::textures::NewFrame()
{
    // The frame ending is the one after the late reads of the frame before:
    // bound in it, or not.
    size_t kept = 0;
    for (const State::LateRead& read : g.lateReads)
    {
        if (read.frame == g.frame) { g.lateReads[kept++] = read; continue; }
        const Image* image = Find(read.id);
        if (image && image->lastBound > read.frame) { g.lateReadsBoundAgain++; continue; }
        g.lateReadsOnLastUse++;
        if (image && (g.lateReadsOnLastUse <= 24 || FlashHunt()))
            LOGW("textures:   re-read on its last use, %ux%u fmt %u at %08X, frame %llu: its"
                 " memory changed after its draws were parsed, and it is not bound again",
                 image->width, image->height, image->xenosFormat, image->address,
                 (unsigned long long)read.frame);
    }
    g.lateReads.resize(kept);
    g.frame++;
    g.epoch++;
    if (!g.device) return;
    // Every half minute, what the cache holds and how much of it is in use.
    if constexpr (diag::kOn)
        if (g.frame % 1800 == 0)
        {
            uint64_t held = 0, recent = 0, recentBytes = 0;
            for (const Image& image : g.images)
            {
                if (!image.image || image.borrowed) continue;
                held++;
                if (g.frame - image.lastBound < kUnusedFrames) { recent++; recentBytes += image.bytes; }
            }
            const vk::imagememory::Totals memory = vk::imagememory::Held();
            LOGI("textures: %llu held (%llu MB), %llu of them bound in the last 600 frames (%llu MB);"
                 " %llu blocks of memory (%llu MB), %zu ids",
                 (unsigned long long)held, (unsigned long long)(g.liveBytes >> 20),
                 (unsigned long long)recent, (unsigned long long)(recentBytes >> 20),
                 (unsigned long long)memory.blocks, (unsigned long long)(memory.blockBytes >> 20),
                 g.images.size());
        }
    if (g.frame % kSweepFrames == 0 && g.liveBytes > g.budget) Evict();
    SweepImages();
    DestroyDoomed();
}

void vk::textures::BeginUploads(uint32_t index)
{
    if (!g.device || index >= kUploadSlots) return;
    UploadSlot& slot = g.uploadSlots[index];
    // The GPU is done with everything the slot held.
    for (StagingBuffer& extra : slot.overflow) FreeStaging(extra);
    const bool overflowed = !slot.overflow.empty();
    slot.overflow.clear();
    if (overflowed || !slot.staging.buffer)
    {
        const VkDeviceSize size =
            std::clamp<VkDeviceSize>(std::max(slot.wanted, slot.staging.size * 2),
                                     kStagingLeast, kStagingMost);
        if (size != slot.staging.size)
        {
            FreeStaging(slot.staging);
            if (!MakeStaging(slot.staging, size)) slot.staging = StagingBuffer{};
        }
    }
    vkResetCommandPool(g.device, slot.pool, 0);
    slot.recording = false;
    slot.segment = 0;
    slot.used = slot.wanted = 0;
    g.open = &slot;
}

namespace
{
    // MW2_MARK_EMPTY=1: paint every world texture uploaded from a block with
    // nothing in it flat magenta, so what the counter counts can be seen. A
    // magenta flash is one the log explains; a black one is something else.
    bool MarkEmpty()
    {
        static const bool on = diag::Flag("MW2_MARK_EMPTY");
        return on;
    }

    uint64_t MagentaId()
    {
        if (g.magenta) return g.magenta;
        gpu::TextureData flat;
        flat.ok = true;
        flat.width = flat.height = 4;
        flat.blocksWide = flat.blocksHigh = 4;
        flat.bytesPerBlock = 4;
        flat.expanded = true;
        flat.bytes.resize(4 * 4 * 4);
        for (size_t at = 0; at + 4 <= flat.bytes.size(); at += 4)
        {
            flat.bytes[at + 0] = 0xFF;   // R
            flat.bytes[at + 1] = 0x00;   // G
            flat.bytes[at + 2] = 0xFF;   // B
            flat.bytes[at + 3] = 0xFF;   // A
        }
        const char* error = nullptr;
        // 0x688 is the identity swizzle, XYZW; 0 would read (R,R,R,R) -- white.
        g.magenta = UploadOne(flat, 6 /* 8_8_8_8 */, 0, 0x688, false, &error);
        if (Image* marker = Find(g.magenta)) marker->pinned = true;
        if (g.magenta == vk::textures::kNone)
            LOGW("textures: could not make the magenta marker: %s", error ? error : "?");
        return g.magenta;
    }
}

namespace
{
    // The fetch constant less what only the sampler reads: the clamps, the
    // filters and the anisotropy, and the border colour. Everything else says
    // which bytes the texture is or how the view shows them.
    std::array<uint32_t, 6> ContentOf(const gpu::TextureFetch& fetch)
    {
        std::array<uint32_t, 6> words{};
        for (uint32_t i = 0; i < 6; i++) words[i] = fetch.d[i];
        words[0] &= ~(0x1FFu << 10);   // ClampX, ClampY, ClampZ
        words[3] &= ~(0x1FFu << 19);   // MagFilter, MinFilter, MipFilter, Anisotropy
        words[5] &= ~0x3u;             // BorderColour
        return words;
    }

    // Where a fetch constant's answer goes in State::recentFetches. The words
    // that tell two textures apart are spread over all six, so each is folded
    // in before the one multiply.
    size_t RecentFetchOf(const std::array<uint32_t, 6>& key)
    {
        static_assert(State::kRecentFetches == size_t(1) << 10, "the index is the hash's top ten bits");
        uint64_t words[3];
        std::memcpy(words, key.data(), sizeof words);
        const uint64_t mixed = words[0] ^ std::rotl(words[1], 23) ^ std::rotl(words[2], 47);
        return size_t((mixed * 0x9E3779B97F4A7C15ull) >> 54);
    }
}

uint64_t vk::textures::Upload(const gpu::TextureFetch& fetch, const char** error)
{
    if (!g.device && !Initialise()) { if (error) *error = "no device"; return kNone; }

    std::array<uint32_t, 6> key{};
    for (uint32_t i = 0; i < 6; i++) key[i] = fetch.d[i];
    State::RecentFetch& recent = g.recentFetches[RecentFetchOf(key)];
    bool known = recent.generation == g.fetchGeneration && recent.key == key;
    if (!known)
        if (auto found = g.byFetch.find(key); found != g.byFetch.end())
        {
            recent = { key, found->second, g.fetchGeneration };
            known = true;
        }
    if (known)
    {
        const uint64_t named = recent.id;
        if (Image* image = Find(named))
        {
            if (image->frame != gpu::replace::kStill && image->lastBound != g.frame)
                Animate(*image);
            image->lastBound = g.frame;
            image->boundSegment = g.segment;
        }
        RefreshIfChanged(named & kImageBits);
        return named;
    }

    // The same texture under another sampler state: the image is held already.
    const std::array<uint32_t, 6> content = ContentOf(fetch);
    if (auto held = g.byContent.find(content); held != g.byContent.end())
    {
        Image& image = g.images[size_t(held->second - 1)];
        const uint32_t sampler = SamplerIndexFor(SamplerKey(fetch, image.minLevel));
        if (!sampler) { Fail("no sampler", error); return kNone; }
        if (image.frame != gpu::replace::kStill && image.lastBound != g.frame) Animate(image);
        image.lastBound = g.frame;
        image.boundSegment = g.segment;
        image.names.push_back(key);
        g.shared++;
        // Named before it is looked at again: a look that orphans the image
        // takes this name from it with the others.
        const uint64_t named = g.byFetch[key] =
            held->second | (uint64_t(sampler) << kSamplerShift);
        RefreshIfChanged(held->second);
        return named;
    }

    stutters::Timed timed(stutters::kNewTextures);
    report::Timed spent(report::kTextures);
    // The base and the mip chain are two ranges with an address each.
    const gpu::TextureExtents extents = gpu::ExtentsOf(fetch);
    const uint8_t* base = HostAddress(extents.baseAddress);
    const uint8_t* mips = HostAddress(extents.mipAddress);
    // Watched before it is read, so a write after the read shows.
    const uint64_t watchStamp = TrackSource(extents.baseAddress, base ? extents.baseBytes : 0,
                                            extents.mipAddress, mips ? extents.mipBytes : 0);
    gpu::TextureData data = gpu::ReadTexture(fetch, base, mips);
    if (!data.ok)
    {
        Fail(data.error ? data.error : "unreadable", error);
        g.byFetch[key] = kNone;
        return kNone;
    }

    // The player's picture for what was read, when there is one
    // (gpu/texture_replace.h).
    uint64_t skin = 0;
    uint32_t frame = gpu::replace::kStill;
    std::shared_ptr<const gpu::TextureData> replacement;
    if (gpu::replace::On())
    {
        skin = gpu::replace::KeyOf(data, fetch.Format());
        gpu::replace::Dump(skin, data, fetch);
        replacement = gpu::replace::Load(skin, data, fetch, SampledAsGamma(fetch), &frame);
        if (!replacement) skin = 0;
    }

    // Only the ranges actually read are watched.
    const size_t baseBytes = base ? extents.baseBytes : 0;
    const size_t mipBytes = mips && data.levels.size() > 1 ? extents.mipBytes : 0;
    const uint64_t signature =
        SignSource(extents.baseAddress, baseBytes, extents.mipAddress, mipBytes);
    // `picture` is the replacement, or null for the title's own bytes. A
    // replacement has every level, whichever of the title's have arrived.
    const auto make = [&](const gpu::TextureData* picture) {
        return UploadOne(data, fetch.Format(), SamplerKey(fetch, picture ? 0 : data.minLevel),
                         fetch.Swizzle(), fetch.Dimension() == gpu::TextureDimension::D1, error,
                         signature, baseBytes, fetch.Linearises(), picture);
    };
    uint64_t id = make(replacement.get());
    if (id == kNone && replacement)
    {
        // Too large for the device, or no memory for it: the title's own
        // texture then, rather than none.
        gpu::replace::Refuse(skin, g.lastError.c_str());
        replacement.reset();
        skin = 0;
        frame = gpu::replace::kStill;
        id = make(nullptr);
    }
    const uint32_t minLevel = skin ? 0 : data.minLevel;
    uint64_t named = id;
    if (Image* uploaded = Find(id))
    {
        uploaded->minLevel = minLevel;
        uploaded->content = content;
        uploaded->names.push_back(key);
        g.byContent[content] = id;
        named |= uint64_t(SamplerIndexFor(SamplerKey(fetch, minLevel))) << kSamplerShift;
        uploaded->address = baseBytes ? extents.baseAddress : 0;
        uploaded->mipAddress = mipBytes ? extents.mipAddress : 0;
        uploaded->mipBytes = mipBytes;
        uploaded->xenosFormat = fetch.Format();
        uploaded->watchStamp = watchStamp;
        std::memcpy(uploaded->fetchWords, fetch.d, sizeof uploaded->fetchWords);
        uploaded->skin = skin;
        uploaded->frame = frame;
        if (skin) gpu::replace::Applied();
        NoteEmptiness(*uploaded, data, EmptyPercent(data.bytes.data(), data.bytes.size()));
    }
    // A failure is cached too: the same six dwords fail the same way, and a
    // texture bound every frame would otherwise fail thousands of times.
    g.byFetch[key] = named;
    return named;
}

namespace
{
    void RefreshRewritten()
    {
        std::vector<uint64_t> again;
        again.swap(g.rewrittenThisSegment);
        RereadEach(again,
            [](uint64_t id, Image& image, std::vector<Reread>& batch) {
                if (!image.sourceBytes && !image.mipBytes) return;
                // What a bind of this segment found rewritten and left to here
                // (RefreshIfChanged). Not written since, its signature is the
                // one taken then and only the read is still owed.
                const bool due = image.restageDue;
                image.restageDue = false;
                const bool unchanged = Unchanged(image);
                if (unchanged)
                {
                    g.unchangedByWatch++;
                    if (!due || image.dueSignature == image.signature) return;
                }
                else
                {
                    g.rereadRewritten++;
                    image.watchStamp = TrackImage(image);
                }
                Reread& job = batch.emplace_back(id, image);
                job.boxes = BoxesToRead(image);
                // The bind's own read, made late, is looked over as it would
                // have been there.
                job.measure = due;
                job.signFirst = !unchanged;
                job.held = image.signature;
                job.signature = image.dueSignature;
            },
            [](Reread& job) {
                Image& image = *job.image;
                g.signNanos += job.signNanos;
                if (job.signature == image.signature || !Restage(job)) return;
                image.signature = job.signature;
                if (job.measure)
                {
                    // Counted as the bind's read would have been.
                    g.refreshed++;
                    NoteEmptiness(image, job.data, job.empty);
                    if (job.signature == image.dueSignature) return;
                }
                if constexpr (diag::kOn) g.lateRestages.push_back(job.id);
                g.lateReads.push_back({ job.id, g.frame });
                g.changedAfterFirstBind++;
            });
    }

    void RefreshEmpty()
    {
        std::vector<uint64_t> again;
        again.swap(g.emptyThisSegment);
        RereadEach(again,
            [](uint64_t id, Image& image, std::vector<Reread>& batch) {
                if (!image.fromAnEmptyBlock) return;
                image.watchStamp = TrackImage(image);
                batch.emplace_back(id, image).refill = true;
            },
            [](Reread& job) {
                Image& image = *job.image;
                if (job.data.ok && job.empty <= 25)
                {
                    if (!Restage(job)) return;
                    if constexpr (diag::kOn) g.lateRestages.push_back(job.id);
                    image.signature = job.signature;
                    image.fromAnEmptyBlock = false;
                    g.refilledBeforeSubmit++;
                    return;
                }
                // Reaches the screen with nothing in it. tools/flash_hunt.sh
                // matches these lines with the flashes somebody saw.
                g.stillEmptyAtSubmit++;
                if (g.stillEmptyAtSubmit <= 24 || FlashHunt())
                {
                    char moves[1024];
                    imagepool::DescribeMovesAround(image.address, uint32_t(image.sourceBytes), 16,
                                                   moves, sizeof moves);
                    LOGI("textures:   still empty at submit, %ux%u fmt %u at %08X (%uKB), frame %llu:"
                         " %s", image.width, image.height, image.xenosFormat, image.address,
                         uint32_t(image.sourceBytes / 1024), (unsigned long long)g.frame, moves);
                }
            });
    }
}

uint64_t vk::textures::Epoch() { return g.epoch; }

uint32_t vk::textures::Version(uint64_t id)
{
    const Image* image = Find(id);
    return image && !image->borrowed ? image->staged : 0;
}

void vk::textures::TakeLateRestages(std::vector<uint64_t>& ids)
{
    ids.swap(g.lateRestages);
    g.lateRestages.clear();
}

bool vk::textures::BoundInThisSegment(uint32_t physical, uint32_t size)
{
    // A linear walk, as MemoryWritten's: a few thousand images, a few copies a frame.
    auto overlaps = [&](uint32_t address, size_t bytes) {
        return bytes && address < physical + size && address + bytes > physical;
    };
    for (const Image& image : g.images)
        if (image.image && !image.borrowed && image.boundSegment == g.segment &&
            (overlaps(image.address, image.sourceBytes) ||
             overlaps(image.mipAddress, image.mipBytes)))
            return true;
    return false;
}

void* vk::textures::EndSegment()
{
    const bool last = g.endingSubmission;
    {
        stutters::Timed timed(stutters::kRefreshedTextures);
        report::Timed spent(report::kTextures);
        RefreshRewritten();
        RefreshEmpty();
    }
    g.segment++;
    g.epoch++;
    g.segmentsEnded++;
    UploadSlot* slot = g.open;
    if (last) g.open = nullptr;
    // A segment that uploaded nothing leaves its buffer to the next.
    if (!slot || !slot->recording) return nullptr;
    slot->recording = false;
    const VkCommandBuffer uploads = slot->commands[slot->segment++];
    g.segmentsWithUploads++;
    if (vkEndCommandBuffer(uploads) != VK_SUCCESS) return nullptr;
    return uploads;
}

void* vk::textures::EndSubmission()
{
    g.endingSubmission = true;
    void* uploads = EndSegment();
    g.endingSubmission = false;
    return uploads;
}

uint64_t vk::textures::NoteDrawnInWorld(uint64_t id)
{
    Image* image = Find(id);
    if (!image || !image->fromAnEmptyBlock) return id;
    if (!image->countedDrawn)
    {
        image->countedDrawn = true;
        g.emptyDrawnInWorld++;
    }
    // Only while the texture is young. Some world textures are legitimately
    // empty -- a mask, an unused lighting slot -- and are bound every frame
    // forever, which would paint the screen and hide what is being looked for.
    constexpr uint64_t kMarkFor = 60;
    const bool young = image->emptySince && g.frame - image->emptySince < kMarkFor;
    if (!MarkEmpty() || !young) return id;
    const uint64_t magenta = MagentaId();
    if (magenta == kNone) return id;
    g.markedEmpty++;
    return magenta;
}

uint64_t vk::textures::UploadData(const gpu::TextureData& data, uint32_t xenosFormat,
                                  const char** error)
{
    if (!g.device && !Initialise()) { if (error) *error = "no device"; return kNone; }
    return UploadOne(data, xenosFormat, 0, 0x688, false, error);
}

uint64_t vk::textures::Adopt(uint64_t key, void* view, const gpu::TextureFetch& fetch, uint32_t width,
                             uint32_t height, uint32_t depth)
{
    if (!g.device && !Initialise()) return kNone;
    // A resolve's copy is one level, whatever the fetch constant says.
    const uint32_t sampler = SamplerIndexFor(SamplerKey(fetch, 0));
    if (!sampler || !view) return kNone;

    auto found = g.adopted.find(key);
    const uint64_t id = found != g.adopted.end() ? found->second : NewSlot();
    if (!id) return kNone;
    if (found == g.adopted.end()) g.adopted[key] = id;
    Image& image = g.images[size_t(id - 1)];
    image.borrowed = true;
    image.view = static_cast<VkImageView>(view);
    image.width = width;
    image.height = height;
    image.perTexel[0] = 1.0f / float(width);
    image.perTexel[1] = 1.0f / float(height);
    image.volume = depth != 0;
    image.layers = std::max(depth, 1u);
    image.perTexel[2] = 1.0f / float(image.layers);
    // A copy's view changes only once the GPU is done with the old one
    // (ForgetViews), so nothing pending reads the entry being rewritten.
    if (image.written != image.view) WriteImage(ElementOf(id), image);
    return id | (uint64_t(sampler) << kSamplerShift);
}

void vk::textures::ForgetViews()
{
    for (Image& image : g.images)
        if (image.borrowed) image.written = VK_NULL_HANDLE;
}

uint32_t vk::textures::Handle(uint64_t id, uint8_t kind, const float** perTexel)
{
    const Image* image = Find(id);
    if (image && !image->written) image = nullptr;   // released
    if (image && KindOf(*image) != kind) { image = nullptr; g.kindMismatches++; }
    const Image& sampled = image ? *image : FallbackOf(kind);
    if (perTexel) *perTexel = sampled.perTexel;
    const uint32_t sampler = SamplerSlotOf(id, sampled);
    return (image ? ElementOf(id) : uint32_t(kind)) | (sampler << vk::bindings::kHandleImageBits);
}

void* vk::textures::Set() { return g.bindless; }

bool vk::textures::Readback(uint64_t id, std::vector<uint8_t>& bytes, const char** error)
{
    Image* image = Find(id);
    if (!image) return Fail("no such texture", error);
    // An adopted entry only borrows a view; there is no image here to copy from.
    if (!image->image) return Fail("the texture is borrowed", error);

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (!MakeBuffer(image->bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    buffer, memory))
        return Fail("no readback buffer", error);

    bool ok = false;
    if (VkCommandBuffer cmd = BeginCommands())
    {
        vk::util::Barrier(cmd, image->image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
        const std::vector<VkBufferImageCopy> regions = Regions(*image, 0);
        vkCmdCopyImageToBuffer(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buffer, uint32_t(regions.size()), regions.data());
        vk::util::Barrier(cmd, image->image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        ok = EndCommands();
    }

    if (ok)
    {
        void* mapped = nullptr;
        ok = vkMapMemory(g.device, memory, 0, image->bytes, 0, &mapped) == VK_SUCCESS;
        if (ok)
        {
            bytes.resize(size_t(image->bytes));
            std::memcpy(bytes.data(), mapped, bytes.size());
            vkUnmapMemory(g.device, memory);
        }
    }
    vkDestroyBuffer(g.device, buffer, nullptr);
    vkFreeMemory(g.device, memory, nullptr);
    if (!ok) return Fail("readback failed", error);
    return true;
}

void vk::textures::Report()
{
    if constexpr (!diag::kOn) return;
    if (!g.uploads && !g.failures) return;
    LOGI("textures: %u uploaded (%llu KB), %u failed, %llu descriptors written, %llu bound as"
         " a fallback because the shader declares another kind",
         g.uploads, (unsigned long long)(g.uploadedBytes / 1024), g.failures,
         (unsigned long long)g.descriptorsWritten, (unsigned long long)g.kindMismatches);
    for (const auto& [reason, count] : g.failureReasons)
        LOGI("textures:   %6u  %s", count, reason.c_str());
    if (g.linearisedRefused)
        LOGI("textures: %llu asked to be linearised in a format with no sRGB form",
             (unsigned long long)g.linearisedRefused);
    if (g.uploadsBatched || g.uploadsAlone)
        LOGI("textures: %llu uploads went ahead of the submission that needed them, %llu were"
             " submitted on their own; %llu overflowed their slot's staging",
             (unsigned long long)g.uploadsBatched, (unsigned long long)g.uploadsAlone,
             (unsigned long long)g.stagingOverflows);
    if (g.resigned)
        LOGI("textures: %llu signings (%.1f ms in all), %llu re-staged because the guest had"
             " rewritten them, %llu signed early because the image pool copied onto them",
             (unsigned long long)g.resigned, g.signNanos / 1e6, (unsigned long long)g.refreshed,
             (unsigned long long)g.invalidatedByWrites);
    if (g.unchangedByWatch)
        LOGI("textures: %llu checks answered by the write watch, nothing signed",
             (unsigned long long)g.unchangedByWatch);
    if (g.rereadRewritten)
        LOGI("textures: rewritten textures signed again at a segment's end %llu times; %llu had"
             " changed since the frame's first bind and were re-staged",
             (unsigned long long)g.rereadRewritten, (unsigned long long)g.changedAfterFirstBind);
    LOGI("textures: of those, %llu bound again the frame after, %llu on their last use (the"
         " title had freed them: a frame drawn with whatever replaced them)",
         (unsigned long long)g.lateReadsBoundAgain, (unsigned long long)g.lateReadsOnLastUse);
    if (g.stagedInParts)
        LOGI("textures: %llu re-reads took only the pages written, %llu KB of them in place of"
             " %llu KB",
             (unsigned long long)g.stagedInParts, (unsigned long long)(g.partBytes >> 10),
             (unsigned long long)(g.wholeBytes >> 10));
    LOGI("textures: %llu segments ended, %llu of them with uploads of their own",
         (unsigned long long)g.segmentsEnded, (unsigned long long)g.segmentsWithUploads);
    if (g.emptyDrawnInWorld || g.refilledBeforeSubmit || g.stillEmptyAtSubmit)
        LOGI("textures: %llu world textures drawn from a block that looked empty; read again"
             " at their segment's end, %llu were put right and %llu reached the screen empty",
             (unsigned long long)g.emptyDrawnInWorld, (unsigned long long)g.refilledBeforeSubmit,
             (unsigned long long)g.stillEmptyAtSubmit);
    LOGI("textures: %llu MB held at the end, against a budget of %llu MB; %llu evicted"
         " (%llu MB) in %llu passes", (unsigned long long)(g.liveBytes >> 20),
         (unsigned long long)(g.budget >> 20), (unsigned long long)g.evicted,
         (unsigned long long)(g.evictedBytes >> 20), (unsigned long long)g.evictionPasses);
    LOGI("textures: %llu released (%llu MB) once idle because their memory had been rewritten;"
         " %llu kept, a page of theirs written and their bytes not; %llu fetch constants were"
         " another sampler state of an image already held",
         (unsigned long long)g.stale, (unsigned long long)(g.staleBytes >> 20),
         (unsigned long long)g.staleNotReally, (unsigned long long)g.shared);
    if (g.markedEmpty)
        LOGI("textures: %llu binds painted magenta (MW2_MARK_EMPTY)",
             (unsigned long long)g.markedEmpty);
}

#endif  // MW2_HAVE_VULKAN
