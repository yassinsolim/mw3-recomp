// Resolves: EDRAM copied out to a texture the title reads back, or the
// finished frame copied out for the window.
#include "renderer_state.h"
#include "display_table.h"
#include "presenter.h"
#include "texture_cache.h"
#include "texture_formats.h"
#include "../texture.h"
#include "../../guest.h"
#include "../../kernel/physical.h"
#include "../../frame_rate.h"
#include "../../log.h"
#include "../../report.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef MW2_HAVE_VULKAN

namespace vk::renderer::detail
{
    // A resolve is a copy in the command stream and nothing more, so it does
    // not end the submission: a frame is submitted once, when it is presented.
    // Ending one at every resolve spent a frame slot on each -- six a frame --
    // so the three slots held half a frame between them, and the CPU waited for
    // the GPU to draw the world while the GPU waited for the CPU to record the
    // next one. Only a slot whose arena share is half used goes early, so the
    // rest of the frame has room.
    bool SubmitIfFilling()
    {
        return g.arenaUsed <= g.arenaSlotBytes / 2 || Submit();
    }

    // The copy this frame's resolves write and this frame's draws read. The
    // first resolve of a frame moves on to the next one; the rest of the frame
    // -- the second shadow cascade, the draws that sample it -- get that same
    // one back.
    Resolved& ResolveCopyFor(uint32_t destination)
    {
        ResolveRing& ring = g.resolvedTo[destination];
        if (ring.frame != g.frames)
        {
            ring.frame = g.frames;
            ring.newest = (ring.newest + 1) % kFrameSlots;
        }
        return ring.copies[ring.newest];
    }

    // `scale` is the surface's the copy comes out of: a copy is as many times
    // the title's size as that surface. `attachment`: a multisampled depth
    // surface is resolved into it (Resolved::attachmentView).
    bool MakeResolveImage(Resolved& into, uint32_t width, uint32_t height, uint32_t scale,
                          VkFormat format, bool depth = false, bool exact = false, bool attachment = false)
    {
        // Grow only within a frame: the second shadow cascade lands inside the
        // surface the first one made, and must not throw the first away.
        //
        // But the first resolve of a frame into this copy (`exact`) sizes it to
        // the surface exactly. The fetch normalises its coordinates over the
        // image, so an image wider than the surface stretches what it holds.
        const bool fits = exact ? into.width == width && into.height == height
                                : into.width >= width && into.height >= height;
        const bool same = into.image && into.format == format && into.depth == depth && into.scale == scale &&
                          (into.attachmentView || !attachment);
        if (same && fits) return true;
        if (!exact && same)
        {
            width = std::max(width, into.width);
            height = std::max(height, into.height);
        }
        // Rare -- a resolve destination that has to grow -- and both a submitted
        // slot and the buffer being recorded may still name the image being
        // replaced.
        if (into.image) RetireBeforeDestroy();
        // The texture cache's set holds these views. Its own check -- the view
        // behind an id changed -- misses a replacement that gets the old handle
        // back, and an entry on the freed image is a texture read of unbacked
        // memory: the TCP page faults that hung the GPU in play and under capture.
        if (into.image) vk::textures::ForgetViews();
        if (into.view) vkDestroyImageView(g.device, into.view, nullptr);
        if (into.gammaView) vkDestroyImageView(g.device, into.gammaView, nullptr);
        if (into.attachmentView) vkDestroyImageView(g.device, into.attachmentView, nullptr);
        if (into.image) vkDestroyImage(g.device, into.image, nullptr);
        if (into.memory) vkFreeMemory(g.device, into.memory, nullptr);
        into = Resolved{};
        g.resolveImagesMade++;

        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width * scale, height * scale, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT;
        if (attachment) info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Two views of one image, so the format has to be allowed to differ from
        // the one the image was made with. The list is what lets a driver keep
        // the compression it would otherwise give up for a mutable image.
        const VkFormat srgb = depth ? VK_FORMAT_UNDEFINED
                                    : VkFormat(vk::formats::SrgbFormatFor(format));
        const VkFormat both[2] = { format, srgb };
        VkImageFormatListCreateInfo list{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
        list.viewFormatCount = 2;
        list.pViewFormats = both;
        if (srgb != VK_FORMAT_UNDEFINED)
        {
            info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            info.pNext = &list;
        }
        if (vkCreateImage(g.device, &info, nullptr, &into.image) != VK_SUCCESS) return false;

        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(g.device, into.image, &needs);
        const uint32_t type = FindMemory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type;
        if (vkAllocateMemory(g.device, &allocate, nullptr, &into.memory) != VK_SUCCESS) return false;
        vkBindImageMemory(g.device, into.image, into.memory, 0);

        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = into.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        // Depth only: a view carrying stencil as well cannot be sampled.
        view.subresourceRange = { VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                           : VK_IMAGE_ASPECT_COLOR_BIT),
                                  0, 1, 0, 1 };
        if (vkCreateImageView(g.device, &view, nullptr, &into.view) != VK_SUCCESS) return false;
        if (attachment)
        {
            view.subresourceRange.aspectMask = kDepthAspects;
            if (vkCreateImageView(g.device, &view, nullptr, &into.attachmentView) != VK_SUCCESS) return false;
        }
        if (srgb != VK_FORMAT_UNDEFINED)
        {
            view.format = srgb;
            if (vkCreateImageView(g.device, &view, nullptr, &into.gammaView) != VK_SUCCESS)
                into.gammaView = VK_NULL_HANDLE;
        }
        into.format = format;
        into.width = width;
        into.height = height;
        into.scale = scale;
        into.depth = depth;
        LOGI("renderer: %ux%u image for a %s resolve destination", width * scale,
             height * scale,
             depth ? "depth" : "colour");
        return true;
    }
}

#ifdef AW_TITLE
namespace vk::renderer::detail
{
    // Advanced Warfare's exposure is worked out by the processor: each frame
    // the scene's luminance is drawn down to 16x16 texels of one float,
    // resolved, and the title reads the texture's memory for the next frame's
    // exposure. A resolve here fills a Vulkan image and guest memory keeps its
    // zeroes, a scene the title takes for pitch dark and opens the exposure
    // all the way for. So that one resolve, and no other (the frame's are
    // megabytes), is also copied into a buffer the host can read and written
    // to the destination once the GPU has finished the submission it went
    // out in. Nothing waits for it: the title gets the luminance a frame or
    // two late (the report says how late), which an adaptation spread over
    // many frames does not show.
    constexpr uint32_t kLuminanceFormat = 36;       // k_32_FLOAT
    constexpr uint32_t kLuminanceTexels = 1024;     // the most one copy holds
    constexpr uint32_t kLuminanceCopies = 16;       // two a frame, a few frames in flight

    struct LuminanceCopy
    {
        uint32_t destination = 0, pitch = 0, x = 0, y = 0, width = 0, height = 0, endian = 0;
        uint32_t place = 0;         // which of the buffer's kLuminanceCopies
        uint32_t slot = 0;
        uint64_t serial = 0, frame = 0;
    };
    std::vector<LuminanceCopy> g_luminanceCopies;

    bool MakeLuminanceBack()
    {
        if (g.luminanceBack) return g.luminanceBackMapped != nullptr;
        const VkDeviceSize bytes = VkDeviceSize(kLuminanceCopies) * kLuminanceTexels * 4;
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(g.device, &info, nullptr, &g.luminanceBack) != VK_SUCCESS) return false;
        VkMemoryRequirements needs{};
        vkGetBufferMemoryRequirements(g.device, g.luminanceBack, &needs);
        const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = FindMemory(needs.memoryTypeBits, visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type == UINT32_MAX) type = FindMemory(needs.memoryTypeBits, visible);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type;
        void* mapped = nullptr;
        if (type == UINT32_MAX ||
            vkAllocateMemory(g.device, &allocate, nullptr, &g.luminanceBackMemory) != VK_SUCCESS ||
            vkBindBufferMemory(g.device, g.luminanceBack, g.luminanceBackMemory, 0) != VK_SUCCESS ||
            vkMapMemory(g.device, g.luminanceBackMemory, 0, bytes, 0, &mapped) != VK_SUCCESS)
        {
            LOGW("renderer: no buffer to read the scene's luminance back into; the exposure stays open");
            return false;
        }
        g.luminanceBackMapped = static_cast<const uint8_t*>(mapped);
        return true;
    }

    // Writes the copies the GPU has finished where their resolves were sent.
    // The texture is tiled, as every resolve destination is, and its dwords
    // are in the order the destination asked for.
    void DeliverLuminance()
    {
        size_t kept = 0;
        for (const LuminanceCopy& copy : g_luminanceCopies)
        {
            if (!SubmissionFinished(copy.slot, copy.serial)) { g_luminanceCopies[kept++] = copy; continue; }
            uint8_t* texture = guest::Base() + kernel::FromPhysical(copy.destination);
            const uint8_t* from = g.luminanceBackMapped + size_t(copy.place) * kLuminanceTexels * 4;
            for (uint32_t y = 0; y < copy.height; y++)
                for (uint32_t x = 0; x < copy.width; x++)
                {
                    uint32_t texel;
                    std::memcpy(&texel, from + (size_t(y) * copy.width + x) * 4, 4);
                    switch (copy.endian)
                    {
                    case 1: texel = ((texel & 0x00FF00FFu) << 8) | ((texel >> 8) & 0x00FF00FFu); break;
                    case 2: texel = __builtin_bswap32(texel); break;
                    case 3: texel = (texel << 16) | (texel >> 16); break;
                    default: break;
                    }
                    std::memcpy(texture + gpu::TiledOffset2D(int32_t(copy.x + x), int32_t(copy.y + y),
                                                             copy.pitch, 2), &texel, 4);
                }
            if (!g.luminanceWrites)
                LOGI("renderer: the scene's luminance (%ux%u floats) is written back to %08X for the title's"
                     " exposure", copy.width, copy.height, copy.destination);
            g.luminanceWrites++;
            g.luminanceFramesLate += g.frames - copy.frame;
        }
        g_luminanceCopies.resize(kept);
    }

    // Called with `image` readable (TRANSFER_SRC_OPTIMAL), in the resolve's
    // own copy. A copy with no place free in the buffer is left out: the
    // title keeps the luminance it last got.
    void ReadLuminanceBack(VkImage image, const gpu::RegisterFile& r, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height)
    {
        const gpu::CopyDestInfo info{ r[gpu::RB_COPY_DEST_INFO] };
        const gpu::CopyDestPitch pitch{ r[gpu::RB_COPY_DEST_PITCH] };
        if (info.Format() != kLuminanceFormat || width * height > kLuminanceTexels || !MakeLuminanceBack())
            return;
        uint32_t place = 0;
        const auto taken = [&](const LuminanceCopy& copy) { return copy.place == place; };
        while (place < kLuminanceCopies &&
               std::any_of(g_luminanceCopies.begin(), g_luminanceCopies.end(), taken))
            place++;
        if (place == kLuminanceCopies) return;
        VkBufferImageCopy region{};
        region.bufferOffset = VkDeviceSize(place) * kLuminanceTexels * 4;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageOffset = { int32_t(x), int32_t(y), 0 };
        region.imageExtent = { width, height, 1 };
        const VkBuffer buffer = g.luminanceBack;
        Record([=](VkCommandBuffer command) {
            vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        });
        g_luminanceCopies.push_back({ r[gpu::RB_COPY_DEST_BASE] & ~0xFFFu, pitch.Pitch(), x, y, width, height,
                                      info.Endian(), place, g.slot, g.submissionSerial + 1, g.frames });
    }
}
#endif

using namespace vk::renderer::detail;

void vk::renderer::Resolve(const gpu::RegisterFile& r)
{
    Stopwatch watch(g.resolveNanoseconds);
    if (!g.device) return;
#ifdef AW_TITLE
    DeliverLuminance();
#endif
    g.resolveEpoch++;
    const gpu::CopyControl control{ r[gpu::RB_COPY_CONTROL] };
    VkRect2D rect{};
    ScissorOf(r, rect);
    const uint32_t destination = r[gpu::RB_COPY_DEST_BASE] & ~0xFFFu;
    const gpu::SurfaceInfo surface{ r[gpu::RB_SURFACE_INFO] };
    const gpu::ColorInfo colour{ r[gpu::RB_COLOR_INFO] };
    // The title reads the scene's depth back as an ordinary texture: a soft
    // particle fades where it meets geometry, and one that cannot find the depth
    // does not fade at all. Skipping this resolve is why the sprites are opaque
    // and the frame sits under a white wash.
    if (control.FromDepth())
    {
        const gpu::DepthInfo depthInfo{ r[gpu::RB_DEPTH_INFO] };
        Target* from = FindTarget(DepthKey(depthInfo.BaseTile(), uint32_t(g.depthFormat),
                                           surface.MsaaSamples(),
                                           PassScale(r[gpu::RB_DEPTH_INFO], surface.MsaaSamples())));
        if (!from) { Skip("resolve of a depth surface never drawn into"); SubmitIfFilling(); return; }
        if (!g.recording && !BeginFrame()) { Skip("no command buffer"); return; }
        g.resolves++;
        EndPass();
        PassTimesBetween();
        from->rowsRead = std::max(from->rowsRead, uint32_t(rect.offset.y) + rect.extent.height);

        uint32_t width = std::min(rect.extent.width, from->width - uint32_t(rect.offset.x));
        uint32_t height = std::min(rect.extent.height, from->height - uint32_t(rect.offset.y));

        // The sun shadow map is one 1024x2048 surface holding two cascades, and
        // the title fills it with two resolves: the first to the surface's base,
        // the second to its lower half, four megabytes on, describing itself as
        // a surface of its own. The world's shaders then bind the whole thing,
        // at the first address, as one texture. Keeping an image per resolve
        // address gave that fetch the first cascade alone, stretched over the
        // lookup for both -- every shadow the near cascade casts landed twice as
        // far down the atlas as it should, and the far cascade was never read.
        // So a resolve that lands inside a surface an earlier resolve this frame
        // started is copied into that surface's image, at the row it lands on.
        const gpu::CopyDestPitch destPitch{ r[gpu::RB_COPY_DEST_PITCH] };
        const gpu::CopyDestInfo destInfo{ r[gpu::RB_COPY_DEST_INFO] };
        const uint32_t texelBytes = std::max(1u, gpu::TextureFormatOf(destInfo.Format()).bitsPerBlock / 8);
        const uint32_t rowBytes = destPitch.Pitch() * texelBytes;
        uint32_t surfaceBase = destination, row = 0;
        for (const auto& [base, known] : g.resolveSurfaces)
            if (known.frame == g.frames && base < destination &&
                destination - base < known.bytes && known.rowBytes == rowBytes && rowBytes &&
                (destination - base) % rowBytes == 0)
            {
                surfaceBase = base;
                row = (destination - base) / rowBytes;
                break;
            }
        if (surfaceBase == destination && rowBytes)
            g.resolveSurfaces[destination] = { rowBytes * std::min(destPitch.Height(), 8192u),
                                               rowBytes, g.frames };

        // A resolve lands where its rectangle sits: one that starts 16 rows down
        // the EDRAM surface starts 16 rows down the texture too, as Xenia's
        // copy_dest_coordinate_info has it. The shadow cascades are resolved
        // from the draw scissor rounded down to 8, which moves with the camera;
        // writing them at row 0 moved every shadow the atlas casts by that many
        // texels, and by a different amount as the view turned.
        const uint32_t destX = uint32_t(rect.offset.x);
        const uint32_t destY = row + uint32_t(rect.offset.y);

        Resolved& into = ResolveCopyFor(surfaceBase);
        // Sized for the whole surface, so the lower half has somewhere to go.
        const uint32_t imageWidth = std::max(destX + width, std::min(destPitch.Pitch(), 4096u));
        const uint32_t imageHeight = std::max(destY + height,
                                              std::min(destPitch.Height(), 4096u));
        const bool firstThisFrame = into.writtenFrame != g.frames;
        // A multisampled surface is resolved straight into the copy, which a
        // rendering instance's resolve can do only where the rectangle sits
        // in the surface: at its first row (the scene's depth, every frame).
        // The copy has to be an attachment for it, and one that is not is
        // made again only at the frame's first resolve into it: later, that
        // would lose what the frame's earlier resolves put there. A copy
        // landing further down, or into such an image, goes through the
        // surface's twin.
        const bool direct = from->samples != VK_SAMPLE_COUNT_1_BIT && row == 0 &&
                            (firstThisFrame || into.attachmentView);
        if (!destination || !width || !height ||
            !MakeResolveImage(into, imageWidth, imageHeight, from->scale, from->format, true, firstThisFrame,
                              direct))
        {
            Skip("depth resolve destination image");
            SubmitIfFilling();
            return;
        }
        width = std::min(width, into.width > destX ? into.width - destX : 0u);
        height = std::min(height, into.height > destY ? into.height - destY : 0u);
        const bool keep = into.writtenFrame == g.frames;
        into.writtenFrame = g.frames;
        // The second cascade keeps the first: it comes out of the layout the
        // first left it in rather than out of UNDEFINED, which may discard.
        const VkImageLayout before = keep ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;

        if (direct)
            ResolveDepthInto(*from, rect.offset, width, height, into.image, into.attachmentView, before,
                             kShaderStages, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                             kShaderStages);
        else
        {
            VkImage fromImage = BeginReadingDepth(*from, rect.offset, width, height);
            if (!fromImage) { Skip("depth resolve of a multisampled surface"); SubmitIfFilling(); return; }
            // Layouts change for both aspects together.
            RecordBarrier(into.image, kDepthAspects, before, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          0, VK_ACCESS_TRANSFER_WRITE_BIT, kShaderStages, VK_PIPELINE_STAGE_TRANSFER_BIT);

            // A copy rather than a blit: depth cannot be filtered on the way
            // through, and a plain copy asks nothing of the format beyond being
            // transferable. Of the depth alone: the copy is sampled through a
            // depth-only view, so its stencil is never read.
            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
            copy.srcOffset = Scaled(rect.offset.x, rect.offset.y, 0, from->scale);
            copy.dstSubresource = copy.srcSubresource;
            copy.dstOffset = Scaled(int32_t(destX), int32_t(destY), 0, from->scale);
            copy.extent = { width * from->scale, height * from->scale, 1 };
            const VkImage intoImage = into.image;
            Record([=](VkCommandBuffer command) {
                vkCmdCopyImage(command, fromImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               intoImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            });

            RecordBarrier(into.image, kDepthAspects,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, kShaderStages);
            EndReadingDepth(*from);
        }
        PassTimesMark("depth resolve %ux%u %ux", width * from->scale, height * from->scale, unsigned(from->samples));
        g.depthResolves++;
        ClearAfterResolve(r, control);
        PassTimesMark("clears after a resolve");
        if (!SubmitIfFilling()) Skip("frame submission failed");
        return;
    }

    // The registers name the surface, and the last pass recorded is not always
    // it: the world's is resolved after a later pass has bound its own.
    const TargetKey sourceKey{ colour.BaseTile(), surface.Pitch(),
                               uint32_t(ColourFormatFor(colour.Format())),
                               surface.MsaaSamples(), false,
                               PassScale(r[gpu::RB_DEPTH_INFO], surface.MsaaSamples()) };
    Target* source = FindTarget(sourceKey);
    if (!source) { Skip("resolve of a surface never drawn into"); SubmitIfFilling(); return; }

    // A resolve can arrive with nothing recording -- the one before it may have
    // submitted a full arena share. Its surface is still there.
    if (!g.recording && !BeginFrame()) { Skip("no command buffer"); return; }
    g.resolves++;

    EndPass();
    PassTimesBetween();
    source->rowsRead = std::max(source->rowsRead, uint32_t(rect.offset.y) + rect.extent.height);

    // The image is the destination surface RB_COPY_DEST_PITCH describes, not
    // the rectangle, and the rectangle lands at its own offset in it -- as
    // the depth resolve above does. The depth of field resolves a 256x152
    // rectangle, rounded up to 8 rows, into a 256x150 surface and fetches
    // 256x150; an image the size of the rectangle put two rows of whatever
    // the target held below the picture, the fetch's v = 1 reached them, and
    // their alpha of 1 -- the blur weight -- spread up through every blur
    // pass into a dark bar along the bottom of the screen when aiming down
    // the sights.
    const gpu::CopyDestPitch destPitch{ r[gpu::RB_COPY_DEST_PITCH] };
    const uint32_t destX = uint32_t(rect.offset.x);
    const uint32_t destY = uint32_t(rect.offset.y);
    uint32_t surfaceWidth = destPitch.Pitch() ? std::min(destPitch.Pitch(), 4096u)
                                              : destX + rect.extent.width;
    if (auto fetched = g.fetchedWidth.find(destination);
        fetched != g.fetchedWidth.end() && fetched->second > destX && fetched->second < surfaceWidth)
        surfaceWidth = fetched->second;
    const uint32_t surfaceHeight = destPitch.Height() ? std::min(destPitch.Height(), 4096u)
                                                      : destY + rect.extent.height;
    const auto room = [](uint32_t size, uint32_t at) { return size > at ? size - at : 0u; };
    const uint32_t width = std::min({ rect.extent.width, room(surfaceWidth, destX),
                                      room(source->width, destX) });
    const uint32_t height = std::min({ rect.extent.height, room(surfaceHeight, destY),
                                       room(source->height, destY) });
    Resolved& into = ResolveCopyFor(destination);
    const bool firstThisFrame = into.writtenFrame != g.frames;
    if (!destination || !width || !height ||
        !MakeResolveImage(into, surfaceWidth, surfaceHeight, source->scale, source->format, false,
                          firstThisFrame))
    {
        Skip("resolve destination image");
        SubmitIfFilling();
        return;
    }
    into.writtenFrame = g.frames;
    into.from = sourceKey;
    // After the draws that sampled this copy's last contents.
    RecordBarrier(into.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT, kShaderStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // The copy is of the surface's format and the rectangle lands where it
    // sits, so a multisampled surface is resolved straight into it, and a
    // single-sampled one copied.
    const VkOffset2D at{ int32_t(destX), int32_t(destY) };
    if (source->samples != VK_SAMPLE_COUNT_1_BIT)
        ResolveColourInto(*source, at, width, height, into.image);
    else
    {
        const VkImage sourceImage = BeginReadingColour(*source, at, width, height);
        VkImageCopy copy{};
        copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.srcOffset = Scaled(int32_t(destX), int32_t(destY), 0, source->scale);
        copy.dstSubresource = copy.srcSubresource;
        copy.dstOffset = copy.srcOffset;
        copy.extent = { width * source->scale, height * source->scale, 1 };
        const VkImage intoImage = into.image;
        Record([=](VkCommandBuffer command) {
            vkCmdCopyImage(command, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           intoImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        });
#ifdef AW_TITLE
        if (source->format == VK_FORMAT_R32_SFLOAT && source->scale == 1)
            ReadLuminanceBack(sourceImage, r, destX, destY, width, height);
#endif
        EndReadingColour(*source);
    }
    RecordBarrier(into.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, kShaderStages);
    PassTimesMark("colour resolve %ux%u %ux", width * source->scale, height * source->scale, unsigned(source->samples));
    ClearAfterResolve(r, control);
    PassTimesMark("clears after a resolve");
    if (!SubmitIfFilling()) Skip("frame submission failed");
}

#ifdef COD4_TITLE
namespace
{
    void (*g_deliverCounts)(uint32_t tag, uint64_t samples) = nullptr;

    // Hands over the occlusion counts the GPU has finished; whether one it
    // has been given is still to come.
    bool DeliverCounts()
    {
        vk::renderer::CollectOcclusionQueries(g_deliverCounts);
        for (const State::PendingQuery& query : g.pendingQueries)
            if (query.serial <= g.submissionSerial) return true;
        return false;
    }
}
#endif

// The frame is whatever the title swaps to: the front buffer the swap packet
// names, which a resolve filled like any other destination. A resolve the size
// of the display is not the frame for being that size -- the title keeps the
// screen in one for the next frame's blur, before the interface is drawn.
void vk::renderer::Swap(uint32_t frontBuffer, [[maybe_unused]] void (*deliver)(uint32_t tag, uint64_t samples))
{
    if (!g.device) return;
    auto found = g.resolvedTo.find(frontBuffer & ~0xFFFu);
    if (found == g.resolvedTo.end()) return;
    Resolved& frame = found->second.copies[found->second.newest];
    if (!frame.image || frame.depth) return;
    if (!g.recording && !BeginFrame()) return;
    EndPass();
    if (Target* source = FindTarget(frame.from)) source->presented++;

    // Copying the frame into an image of its own, rather than handing the
    // resolve's to the window thread, keeps the next frame from overwriting it.
    const uint32_t index = g.presentIndex;
    VkImage present = g.present[index];
    // Paced by the display (frame_rate.h), this frame is not finished until
    // the window has taken the one before it: the window takes one a blank,
    // so the renderer, and the title behind it, run at the display's rate
    // with one frame waiting at most. The image's own last frame is older
    // and was taken before that one.
    if (frame_rate::Mode() == frame_rate::kByDisplay)
    {
        report::Timed waited(report::kDisplayWait);
        const uint64_t before = g.presentSerial[(index + kPresentImages - 1) % kPresentImages];
#ifdef COD4_TITLE
        // Call of Duty 4's back end waits awake for an earlier frame's
        // occlusion count of the sun (sub_82108830: GetData and Sleep(0)
        // until it is there), and the count is handed over on this thread,
        // which is here for most of a frame at 120: the back end ran a core
        // at 90% doing nothing, and could not start on the next frame before
        // this one was shown. So the counts are handed over during the wait.
        // Modern Warfare 2 and 3 were measured without, and are left so.
        g_deliverCounts = deliver;
        vk::WaitForDisplay(before, DeliverCounts() ? DeliverCounts : nullptr);
#else
        vk::WaitForDisplay(before);
#endif
    }
    else vk::WaitUntilTaken(g.presentSerial[index]);

    RecordBarrier(frame.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  kShaderStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // After the window's copy out of it, and the dumps' and the display
    // table's work on it, the last time round.
    RecordBarrier(present, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = Scaled(int32_t(frame.width), int32_t(frame.height), 1, frame.scale);
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = { int32_t(g.presentWidth), int32_t(g.presentHeight), 1 };
    const VkImage frameImage = frame.image;
    Record([=](VkCommandBuffer command) {
        vkCmdBlitImage(command, frameImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       present, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    });
    RecordBarrier(frame.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, kShaderStages);

    // What a television is handed has been through the display's colour table,
    // so what the window and the frame dumps show has to be too.
    vk::display::Apply(present, g.presentWidth, g.presentHeight,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    RecordBarrier(present, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    FinishFrame(present);
}

#endif  // MW2_HAVE_VULKAN
