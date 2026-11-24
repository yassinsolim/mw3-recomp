// EDRAM surfaces as Vulkan images: making them, reading them back, the
// rendering instances they are drawn in, and clearing them.
#include "renderer_state.h"
#include "../../log.h"

#include <algorithm>

#ifdef MW2_HAVE_VULKAN

namespace vk::renderer::detail
{
    bool MakeImage(Target& target, VkImageUsageFlags usage, VkImageAspectFlags aspect)
    {
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = target.format;
        info.extent = { target.width * target.scale, target.height * target.scale, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = target.samples;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(g.device, &info, nullptr, &target.image) != VK_SUCCESS)
        {
            target.image = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(g.device, target.image, &needs);
        const uint32_t type = FindMemory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX)
        {
            vkDestroyImage(g.device, target.image, nullptr);
            target.image = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type;
        if (vkAllocateMemory(g.device, &allocate, nullptr, &target.memory) != VK_SUCCESS)
        {
            vkDestroyImage(g.device, target.image, nullptr);
            target.image = VK_NULL_HANDLE;
            target.memory = VK_NULL_HANDLE;
            return false;
        }
        if (vkBindImageMemory(g.device, target.image, target.memory, 0) != VK_SUCCESS)
        {
            vkFreeMemory(g.device, target.memory, nullptr);
            vkDestroyImage(g.device, target.image, nullptr);
            target.image = VK_NULL_HANDLE;
            target.memory = VK_NULL_HANDLE;
            return false;
        }

        if (aspect)
        {
            VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view.image = target.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = target.format;
            view.subresourceRange = { aspect, 0, 1, 0, 1 };
            if (vkCreateImageView(g.device, &view, nullptr, &target.view) != VK_SUCCESS)
            {
                target.view = VK_NULL_HANDLE;
                vkFreeMemory(g.device, target.memory, nullptr);
                vkDestroyImage(g.device, target.image, nullptr);
                target.image = VK_NULL_HANDLE;
                target.memory = VK_NULL_HANDLE;
                return false;
            }
        }
        return true;
    }

    VkFormat ColourFormatFor(uint32_t format)
    {
        switch (format)
        {
        case 0: case 1: case 10: return VK_FORMAT_R8G8B8A8_UNORM;   // 8_8_8_8 and gamma
        case 2: case 3: case 12: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 4:  return VK_FORMAT_R16G16_SNORM;
        case 5:  return VK_FORMAT_R16G16B16A16_SNORM;
        case 6:  return VK_FORMAT_R16G16_SFLOAT;
        case 7:  return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 14: return VK_FORMAT_R32_SFLOAT;
        case 15: return VK_FORMAT_R32G32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
        }
    }

    // EDRAM has one depth buffer, not one per way of describing it. A pass that
    // writes depth at 1040 wide without multisampling and a pass that tests it at
    // 1040 wide with two samples are the same tiles on the hardware; keeping an
    // image per (pitch, samples) gives the second pass a buffer the first never
    // wrote, and MW2 draws its whole world in the second pass. So the pitch is
    // not part of a depth target's key.
    //
    // With the surfaces drawn multisampled the sample count stays in the key:
    // a depth image has one sample count, and the colour target it is bound
    // beside has to share it. MW2 tests and writes its world depth at 2x in
    // both passes, so nothing is split by that.
    TargetKey DepthKey(uint32_t baseTile, uint32_t format, uint32_t samples, uint32_t scale)
    {
        return { baseTile, 0, format, g.msaa ? samples : 0, true, scale };
    }

    // The console's 1x, 2x and 4x, as the device can draw them. A count the
    // device lacks is rounded up to the next one it has; 1x is the last resort.
    //
    // MW2_MSAA=<n> draws every surface the title multisamples at n instead --
    // 8 on most devices -- for seeing that it works: at the console's own 2x,
    // upscaled from 1024x600 to the window with a linear blit, the difference is
    // slight. A count past what the device offers is rounded down to what it has.
    VkSampleCountFlagBits HostSamples(uint32_t guestSamples)
    {
        if (!g.msaa) return VK_SAMPLE_COUNT_1_BIT;
        static const uint32_t forced = uint32_t(env::Number("MW2_MSAA"));
        if (forced > 1 && guestSamples)
        {
            for (uint32_t bit = 64; bit > 1; bit >>= 1)
                if (bit <= forced && (g.sampleCounts & bit)) return VkSampleCountFlagBits(bit);
        }
        for (uint32_t bit = 1u << (guestSamples & 3); bit <= 64; bit <<= 1)
            if (g.sampleCounts & bit) return VkSampleCountFlagBits(bit);
        return VK_SAMPLE_COUNT_1_BIT;
    }

    // Besides the clears a resolve asks for (ClearAfterResolve), MW2 clears
    // EDRAM by drawing:
    // it points the surface at a base with a pitch and height that span the rest
    // of the memory and draws rectangles over all of it with the depth test
    // ALWAYS and depth writes on. The rectangles are in EDRAM's own coordinates
    // -- the viewport transform is switched off for them -- and they come in
    // pairs, because a tile is 80 samples wide and a surface is not a whole
    // number of tiles: one rectangle covers the columns that divide evenly, a
    // second, describing the same memory at a different pitch and sample count,
    // covers the column left over. Together they name one span of tiles from the
    // surface's base upwards, which is where every depth buffer the frame is
    // about to draw against lives.
    //
    // Here each (base, pitch, samples) is an image of its own, so a rectangle
    // covers whatever part of one image its own coordinates happen to fall in
    // and nothing else. So the idiom is recognised and answered with clears of
    // the images it names, and the rectangles themselves are not drawn. It is not
    // a model of EDRAM, but it is what this particular idiom means.

    void ClearAliasedDepth(uint32_t fromTile)
    {
        bool ended = false;
        for (auto& [key, target] : g.targets)
        {
            if (!target.depth || key.baseTile < fromTile) continue;
            // Nothing has been drawn into it since it was last cleared, so
            // there is nothing to clear. The idiom arrives as a pair of
            // rectangles covering one tile span between them, each pass
            // clears before it draws, and the span reaches every depth
            // surface above its base whichever the pass is about to use:
            // Advanced Warfare's frame cleared seventeen depth images, five
            // of them as tall as a target may be, to draw into three.
            if (target.depthEmpty) { target.depthClearedIn = g.frames; continue; }
            if (!ended) { EndPass(); ended = true; }
            const VkImageAspectFlags aspect =
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            RecordBarrier(target.image, aspect,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          kDepthStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
            // Zero: the level tests GEQUAL and RB_DEPTH_CLEAR is zero.
            const VkClearDepthStencilValue value{ 0.0f, 0 };
            const VkImageSubresourceRange range{ aspect, 0, 1, 0, 1 };
            const VkImage image = target.image;
            Record([=](VkCommandBuffer command) {
                vkCmdClearDepthStencilImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            &value, 1, &range);
            });
            RecordBarrier(target.image, aspect,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, kDepthAccess,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, kDepthStages);
            target.depthEmpty = true;
            target.depthClearedIn = g.frames;
            PassTimesMark("depth cleared by the title's idiom %ux%u %ux", target.width * target.scale,
                          target.height * target.scale, unsigned(target.samples));
        }
    }

    // A colour target filled with one value, outside any pass. `packed` is
    // the colour as the 8_8_8_8 family stores it: ARGB, most significant byte
    // first.
    void ClearColour(Target& target, uint32_t packed)
    {
        VkClearColorValue value{};
        value.float32[0] = ((packed >> 16) & 0xFF) / 255.0f;
        value.float32[1] = ((packed >>  8) & 0xFF) / 255.0f;
        value.float32[2] = ((packed >>  0) & 0xFF) / 255.0f;
        value.float32[3] = ((packed >> 24) & 0xFF) / 255.0f;
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        RecordBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      kColourStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkImage image = target.image;
        Record([=](VkCommandBuffer command) {
            vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &value, 1, &range);
        });
        RecordBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, kColourAccess,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, kColourStages);
    }

    // D3D's Clear of a colour surface is the depth-only idiom too: it
    // describes the colour surface's own tiles as a D24S8 depth surface, at
    // half the pitch and 4x, and draws one rectangle over them with the depth
    // test ALWAYS and the stencil operation REPLACE. The 32 bits a sample then
    // holds, depth << 8 | stencil reference, are the clear colour: EDRAM does
    // not know which kind of surface a tile belongs to. MW3's interface is
    // cleared this way and no other, once a frame; without it a menu is drawn
    // over whatever the menus before it left.
    //
    // A colour image here is cleared when the rectangle's rows of tiles are
    // its own: the same base tile, as many tiles a row, and all its rows.
    // `packed` is what each sample is left holding.
    void ClearAliasedColour(uint32_t baseTile, uint32_t tilesPerRow, uint32_t rows, uint32_t packed)
    {
        bool ended = false;
        for (auto& [key, target] : g.targets)
        {
            if (target.depth || key.baseTile != baseTile) continue;
            // The formats whose 32 bits ClearColour reads.
            if (target.format != VK_FORMAT_R8G8B8A8_UNORM) continue;
            // A tile is 80 samples by 16; 2x doubles the rows, 4x the columns too.
            const uint32_t across = key.pitch << (key.samples == 2 ? 1 : 0);
            const uint32_t down = target.height << (key.samples ? 1 : 0);
            if ((across + 79) / 80 != tilesPerRow || (down + 15) / 16 > rows) continue;
            if (!ended) { EndPass(); ended = true; }
            ClearColour(target, packed);
            PassTimesMark("colour cleared by the title's idiom %ux%u %ux", target.width * target.scale,
                          target.height * target.scale, unsigned(target.samples));
        }
    }

    Target* FindTarget(const TargetKey& key)
    {
        auto found = g.targets.find(key);
        return found == g.targets.end() ? nullptr : &found->second;
    }

    void DestroySingle(Target& target)
    {
        if (target.singleView) vkDestroyImageView(g.device, target.singleView, nullptr);
        if (target.singleImage) vkDestroyImage(g.device, target.singleImage, nullptr);
        if (target.singleMemory) vkFreeMemory(g.device, target.singleMemory, nullptr);
        target.singleView = VK_NULL_HANDLE;
        target.singleImage = VK_NULL_HANDLE;
        target.singleMemory = VK_NULL_HANDLE;
    }

    void DestroyTarget(Target& target)
    {
        InvalidateFramebuffers();
        if (target.view) vkDestroyImageView(g.device, target.view, nullptr);
        if (target.image) vkDestroyImage(g.device, target.image, nullptr);
        if (target.memory) vkFreeMemory(g.device, target.memory, nullptr);
        DestroySingle(target);
    }

    // The single-sampled twin of a multisampled target, made on first use. A
    // colour twin is a transfer destination the resolve fills; a depth twin is
    // an attachment, because a depth resolve only happens inside a render pass.
    bool EnsureSingle(Target& target)
    {
        if (target.singleImage) return true;
        Target one = target;
        one.samples = VK_SAMPLE_COUNT_1_BIT;
        one.image = VK_NULL_HANDLE;
        one.memory = VK_NULL_HANDLE;
        one.view = VK_NULL_HANDLE;
        const VkImageUsageFlags usage = target.depth
            ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
            : (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        // A view only for the depth twin, which is bound as an attachment; a
        // transfer-only image may not have one.
        const VkImageAspectFlags aspect = target.depth
            ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
            : 0;
        if (!MakeImage(one, usage, aspect))
        {
            if (one.view) vkDestroyImageView(g.device, one.view, nullptr);
            if (one.image) vkDestroyImage(g.device, one.image, nullptr);
            if (one.memory) vkFreeMemory(g.device, one.memory, nullptr);
            return false;
        }
        target.singleImage = one.image;
        target.singleMemory = one.memory;
        target.singleView = one.view;
        return true;
    }

    // The rectangle of a multisampled colour target resolved into `into`, an
    // image of its format in TRANSFER_DST, at the same place. Outside any
    // render pass; the target is an attachment again afterwards.
    void ResolveColourInto(Target& target, VkOffset2D at, uint32_t width, uint32_t height, VkImage into)
    {
        RecordBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      kColourStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageResolve region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.srcOffset = Scaled(at.x, at.y, 0, target.scale);
        region.dstSubresource = region.srcSubresource;
        region.dstOffset = region.srcOffset;
        region.extent = { width * target.scale, height * target.scale, 1 };
        const VkImage from = target.image;
        Record([=](VkCommandBuffer command) {
            vkCmdResolveImage(command, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              into, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        });
        RecordBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, kColourAccess,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, kColourStages);
    }

    // What a copy out of a colour target reads, in TRANSFER_SRC. For a
    // single-sampled target that is the target itself; for a multisampled one
    // the rectangle is resolved into the twin first. Outside any render pass.
    VkImage BeginReadingColour(Target& target, VkOffset2D at, uint32_t width, uint32_t height)
    {
        if (target.samples == VK_SAMPLE_COUNT_1_BIT)
        {
            RecordBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                          kColourStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
            return target.image;
        }
        if (!EnsureSingle(target)) return VK_NULL_HANDLE;
        // From UNDEFINED: only the rectangle is read afterwards. After the
        // copies out of the twin's last contents.
        RecordBarrier(target.singleImage, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        ResolveColourInto(target, at, width, height, target.singleImage);
        RecordBarrier(target.singleImage, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        return target.singleImage;
    }

    void EndReadingColour(Target& target)
    {
        if (target.samples != VK_SAMPLE_COUNT_1_BIT) return;   // never left the attachment layout
        RecordBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, kColourAccess,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, kColourStages);
    }

    // The same for a depth target, whose resolve is a render pass over the
    // rectangle with nothing drawn in it.
    VkImage BeginReadingDepth(Target& target, VkOffset2D at, uint32_t width, uint32_t height)
    {
        if (target.samples == VK_SAMPLE_COUNT_1_BIT)
        {
            RecordBarrier(target.image, kDepthAspects,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                          kDepthStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
            return target.image;
        }
        if (!EnsureSingle(target)) return VK_NULL_HANDLE;
        // The copies before it read the twin, which comes out in
        // TRANSFER_SRC, ready to be copied from.
        ResolveDepthInto(target, at, width, height, target.singleImage, target.singleView,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
        return target.singleImage;
    }

    // A rendering instance with nothing drawn in it, which loads the
    // multisampled depth and resolves the rectangle into `into` at the same
    // place: a transfer cannot. The first sample, as the console's own depth
    // resolve takes one sample rather than an average -- an averaged depth is
    // a surface nothing was drawn at. The stencil is attached too, loaded and
    // stored, so the target's stays what it was: a view of both aspects
    // attached as depth alone would leave it to how a driver keeps the two
    // together. Nothing reads a resolved stencil, so it is not resolved where
    // the device allows (State::stencilUnresolved), and resolved alike where
    // not. The target stays an attachment throughout; `into` goes from
    // `before` (last used by `usedBy`) to `after`, for `readAs` at `readBy`.
    void ResolveDepthInto(Target& target, VkOffset2D at, uint32_t width, uint32_t height,
                          VkImage into, VkImageView intoView, VkImageLayout before, VkPipelineStageFlags usedBy,
                          VkImageLayout after, VkAccessFlags readAs, VkPipelineStageFlags readBy)
    {
        const VkImageView view = target.view;
        const VkRect2D area{ Scaled(at, target.scale), Scaled(VkExtent2D{ width, height }, target.scale) };
        const bool stencilUnresolved = g.stencilUnresolved;
        Record([=](VkCommandBuffer command) {
            // The draws before it wrote the depth.
            VkMemoryBarrier written{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            written.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            written.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            VkImageMemoryBarrier entering{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            entering.srcAccessMask = 0;
            entering.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            entering.oldLayout = before;
            entering.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            entering.srcQueueFamilyIndex = entering.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            entering.image = into;
            entering.subresourceRange = { kDepthAspects, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(command, kDepthStages | usedBy,
                                 kDepthStages | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 1, &written, 0, nullptr, 1, &entering);

            VkRenderingAttachmentInfoKHR depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
            depth.imageView = view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            depth.resolveImageView = intoView;
            depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfoKHR info{ VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
            info.renderArea = area;
            info.layerCount = 1;
            VkRenderingAttachmentInfoKHR stencil = depth;
            if (stencilUnresolved)
            {
                stencil.resolveMode = VK_RESOLVE_MODE_NONE;
                stencil.resolveImageView = VK_NULL_HANDLE;
            }
            info.pDepthAttachment = &depth;
            info.pStencilAttachment = &stencil;
            if (dispatch.beginRendering && dispatch.endRendering)
            {
                dispatch.beginRendering(command, &info);
                dispatch.endRendering(command);
            }

            // A resolve writes at the colour output stage whatever it resolves.
            VkImageMemoryBarrier leaving = entering;
            leaving.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            leaving.dstAccessMask = readAs;
            leaving.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            leaving.newLayout = after;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthStages,
                                 readBy, 0, 0, nullptr, 0, nullptr, 1, &leaving);
        });
    }

    void EndReadingDepth(Target& target)
    {
        if (target.samples != VK_SAMPLE_COUNT_1_BIT) return;
        RecordBarrier(target.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, kDepthAccess,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, kDepthStages);
    }

    Target* EnsureTarget(const TargetKey& key, uint32_t width, uint32_t height,
                         uint32_t guestFormat)
    {
#if defined(MW2_ANDROID) || defined(__ANDROID__)
        // A phone's memory: no surface larger than this either way.
        constexpr uint32_t kLargestSide = 2048;
#else
        constexpr uint32_t kLargestSide = UINT32_MAX;
#endif
        width = std::min(width, kLargestSide);
        height = std::min(height, kLargestSide);
        State::LastTarget& last = g.lastTargets[key.depth];
        if (last.target && last.key == key && last.target->width >= width && last.target->height >= height)
            return last.target;
        auto found = g.targets.find(key);
        if (found != g.targets.end() &&
            found->second.width >= width && found->second.height >= height)
        {
            last = { key, &found->second };
            return &found->second;
        }

        if (found != g.targets.end())
        {
            // The surface grew. Rare enough to rebuild rather than plan for.
            RetireBeforeDestroy();
            DestroyTarget(found->second);
            width = std::min(kLargestSide, std::max(width, found->second.width));
            height = std::min(kLargestSide, std::max(height, found->second.height));
            g.targets.erase(found);
            g.lastTargets[0] = g.lastTargets[1] = {};
        }

        Target target;
        target.depth = key.depth;
        target.scale = key.scale;
        target.width = width;
        target.height = height;
        target.format = VkFormat(key.format);
        target.guestFormat = guestFormat;
        target.samples = HostSamples(key.samples);
        if (target.format == VK_FORMAT_UNDEFINED) { Skip("render target format"); return nullptr; }

        // TRANSFER_DST because targets are cleared with transfer commands --
        // on a new target, after a resolve, and for the EDRAM clear idiom. An
        // image created without it is laid out on the assumption nothing will
        // ever write it that way.
        const VkImageUsageFlags usage = key.depth
            ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        const VkImageAspectFlags aspect = key.depth
            ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
            : VK_IMAGE_ASPECT_COLOR_BIT;
        if (!MakeImage(target, usage, aspect)) { Skip("render target allocation"); return nullptr; }

        LOGI("renderer: %s target %ux%u at EDRAM tile %u (%s), %ux for the title's %ux",
             key.depth ? "depth" : "colour", target.width * target.scale, target.height * target.scale,
             key.baseTile,
             key.depth ? gpu::DepthTargetFormatName(guestFormat)
                       : (gpu::ColorTargetFormatName(guestFormat) ?: "unknown"),
             unsigned(target.samples), 1u << key.samples);
        Target& stored = g.targets.emplace(key, target).first->second;
        ClearNewTarget(stored);
        return &stored;
    }

    VkRenderPass GetRenderPass(VkFormat colourFormat, VkFormat depthFormat, VkSampleCountFlagBits samples)
    {
        static std::mutex s_renderPassMutex;
        std::lock_guard lock(s_renderPassMutex);
        const RenderPassKey key{ colourFormat, depthFormat, samples };
        auto found = g.legacyRenderPasses.find(key);
        if (found != g.legacyRenderPasses.end()) return found->second;

        VkAttachmentDescription attachments[2]{};
        uint32_t count = 0;

        VkAttachmentReference colourRef{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
        if (colourFormat != VK_FORMAT_UNDEFINED)
        {
            colourRef = { count, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkAttachmentDescription& desc = attachments[count++];
            desc.format = colourFormat;
            desc.samples = samples;
            desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            desc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

        VkAttachmentReference depthRef{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
        if (depthFormat != VK_FORMAT_UNDEFINED)
        {
            depthRef = { count, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            VkAttachmentDescription& desc = attachments[count++];
            desc.format = depthFormat;
            desc.samples = samples;
            desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            desc.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            desc.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = (colourFormat != VK_FORMAT_UNDEFINED) ? 1 : 0;
        subpass.pColorAttachments = (colourFormat != VK_FORMAT_UNDEFINED) ? &colourRef : nullptr;
        subpass.pDepthStencilAttachment = (depthFormat != VK_FORMAT_UNDEFINED) ? &depthRef : nullptr;

        // Subpass dependencies to prevent tile memory writeback hazards on TBDR GPUs (Mali, Adreno).
        // Using VK_DEPENDENCY_BY_REGION_BIT ensures dependencies stay within on-chip tile SRAM.
        VkSubpassDependency dependencies[2]{};
        uint32_t depCount = 0;

        if (colourFormat != VK_FORMAT_UNDEFINED)
        {
            VkSubpassDependency& dep = dependencies[depCount++];
            dep.srcSubpass = VK_SUBPASS_EXTERNAL;
            dep.dstSubpass = 0;
            dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        }

        if (depthFormat != VK_FORMAT_UNDEFINED)
        {
            VkSubpassDependency& dep = dependencies[depCount++];
            dep.srcSubpass = VK_SUBPASS_EXTERNAL;
            dep.dstSubpass = 0;
            dep.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        }

        VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        info.attachmentCount = count;
        info.pAttachments = attachments;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = depCount;
        info.pDependencies = dependencies;

        VkRenderPass pass = VK_NULL_HANDLE;
        VkResult r = vkCreateRenderPass(g.device, &info, nullptr, &pass);
        if (r != VK_SUCCESS)
        {
            LOGE("renderer: vkCreateRenderPass failed: %d (colour=%d, depth=%d, samples=%d)",
                 r, colourFormat, depthFormat, samples);
            return VK_NULL_HANDLE;
        }

        g.legacyRenderPasses[key] = pass;
        return pass;
    }

    VkFramebuffer GetFramebuffer(VkRenderPass pass, VkImageView colourView, VkImageView depthView,
                                 uint32_t width, uint32_t height)
    {
        if (!pass) return VK_NULL_HANDLE;
        const FramebufferKey key{ pass, colourView, depthView, width, height };
        auto found = g.legacyFramebuffers.find(key);
        if (found != g.legacyFramebuffers.end()) return found->second;

        VkImageView views[2]{};
        uint32_t count = 0;
        if (colourView) views[count++] = colourView;
        if (depthView) views[count++] = depthView;
        if (!count) return VK_NULL_HANDLE;

        VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        info.renderPass = pass;
        info.attachmentCount = count;
        info.pAttachments = views;
        info.width = width;
        info.height = height;
        info.layers = 1;

        VkFramebuffer fb = VK_NULL_HANDLE;
        VkResult res = vkCreateFramebuffer(g.device, &info, nullptr, &fb);
        if (res != VK_SUCCESS)
        {
            LOGE("renderer: vkCreateFramebuffer failed: %d (pass=%p, count=%u, %ux%u)",
                 res, pass, count, width, height);
            return VK_NULL_HANDLE;
        }

        g.legacyFramebuffers[key] = fb;
        return fb;
    }

    void InvalidateFramebuffers()
    {
        for (auto& [k, fb] : g.legacyFramebuffers)
            if (fb) vkDestroyFramebuffer(g.device, fb, nullptr);
        g.legacyFramebuffers.clear();
    }

    void ClearRenderPassCache()
    {
        InvalidateFramebuffers();
        for (auto& [k, pass] : g.legacyRenderPasses)
            if (pass) vkDestroyRenderPass(g.device, pass, nullptr);
        g.legacyRenderPasses.clear();
    }

    void BeginRendering(const Target& colour, const Target& depth, uint32_t width,
                        uint32_t height)
    {
        EndPass();
        PassTimesOpen(colour, uint32_t(colour.samples));
        const VkImageView colourView = colour.view, depthView = depth.view;
        if (!colourView && !depthView) return;

        // Both are drawn at the pass's scale: their keys carry it.
        const VkExtent2D area = Scaled(VkExtent2D{ width, height }, colour.scale);

        const bool legacy = vk::pipeline::LegacyMode();
        VkRenderPass legacyPass = VK_NULL_HANDLE;
        VkFramebuffer legacyFb = VK_NULL_HANDLE;
        if (legacy)
        {
            const VkFormat cFmt = colourView ? colour.format : VK_FORMAT_UNDEFINED;
            const VkFormat dFmt = depthView ? depth.format : VK_FORMAT_UNDEFINED;
            VkSampleCountFlagBits samples = (cFmt != VK_FORMAT_UNDEFINED) ? colour.samples : depth.samples;
            if (samples == 0) samples = VK_SAMPLE_COUNT_1_BIT;
            legacyPass = GetRenderPass(cFmt, dFmt, samples);
            legacyFb = GetFramebuffer(legacyPass, colourView, depthView, area.width, area.height);
            if (!legacyPass || !legacyFb)
            {
                LOGE("renderer: BeginRendering failed to acquire render pass (%p) or framebuffer (%p)",
                     legacyPass, legacyFb);
                return;
            }
        }
        g.pass = PassBegin{ colourView, depthView, area, legacyPass, legacyFb };
        Record([begin = g.pass](VkCommandBuffer command) { begin(command); });
        g.passUnsure = false;
        g.currentScale = colour.scale;
        // A query has to end in the instance it began in, and a boundary
        // still unsettled could close this one under it: it is settled now.
        if (g.guestCounting) ReleaseGate();
        OpenGuestQuery();
        g.currentColour = colour.view;
        g.currentDepth = depth.view;
        g.currentSamples = uint32_t(colour.samples);
        g.currentWidth = width;
        g.currentHeight = height;
    }

    void PassBegin::operator()(VkCommandBuffer command) const
    {
        // What a render pass's dependency in from the outside was: a frame
        // is many passes over the same attachments, with copies and clears
        // between them, and this one waits for their writes.
        VkMemoryBarrier before{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        before.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_TRANSFER_WRITE_BIT;
        before.dstAccessMask = kColourAccess | kDepthAccess;
        vkCmdPipelineBarrier(command,
                             kColourStages | kDepthStages | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             kColourStages | kDepthStages, 0, 1, &before, 0, nullptr, 0,
                             nullptr);
        if (legacyPass)
        {
            VkRenderPassBeginInfo beginInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            beginInfo.renderPass = legacyPass;
            beginInfo.framebuffer = legacyFramebuffer;
            beginInfo.renderArea.extent = area;
            vkCmdBeginRenderPass(command, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        }
        else
        {
            // LOAD, not CLEAR: a frame is many rendering instances -- one per
            // change of target -- and only the title's own draws clear anything.
            VkRenderingAttachmentInfoKHR colourAttachment{
                VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
            colourAttachment.imageView = colour;
            colourAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colourAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            colourAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingAttachmentInfoKHR depthAttachment = colourAttachment;
            depthAttachment.imageView = depth;
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            VkRenderingInfoKHR info{ VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
            info.renderArea.extent = area;
            info.layerCount = 1;
            info.colorAttachmentCount = 1;
            info.pColorAttachments = &colourAttachment;
            info.pDepthAttachment = &depthAttachment;
            info.pStencilAttachment = &depthAttachment;
            dispatch.beginRendering(command, &info);
        }
        RecorderInPass() = true;
    }

    void ResumeRendering()
    {
        g.passUnsure = false;
        Record([begin = g.pass](VkCommandBuffer command) {
            if (!RecorderInPass()) begin(command);
        });
    }

    // The Xenos clears the EDRAM surface as part of the resolve that reads it
    // (bits 8 and 9 of RB_COPY_CONTROL). Without the clear every frame is drawn
    // on top of the last.
    void ClearAfterResolve(const gpu::RegisterFile& r, const gpu::CopyControl& control)
    {
        if (!control.ClearsColor() && !control.ClearsDepth()) return;

        if (control.ClearsColor())
        {
            const gpu::SurfaceInfo surface{ r[gpu::RB_SURFACE_INFO] };
            const gpu::ColorInfo colour{ r[gpu::RB_COLOR_INFO] };
            if (Target* target = FindTarget({ colour.BaseTile(), surface.Pitch(),
                                              uint32_t(ColourFormatFor(colour.Format())),
                                              surface.MsaaSamples(), false,
                                              PassScale(r[gpu::RB_DEPTH_INFO], surface.MsaaSamples()) }))
                ClearColour(*target, r[gpu::RB_COLOR_CLEAR]);
        }

        if (control.ClearsDepth())
        {
            const gpu::SurfaceInfo surface{ r[gpu::RB_SURFACE_INFO] };
            const gpu::DepthInfo depthInfo{ r[gpu::RB_DEPTH_INFO] };
            if (Target* target = FindTarget(DepthKey(depthInfo.BaseTile(), uint32_t(g.depthFormat),
                                                     surface.MsaaSamples(),
                                                     PassScale(r[gpu::RB_DEPTH_INFO], surface.MsaaSamples()))))
            {
                // The stored value is 24-bit depth with stencil in the low byte.
                const uint32_t packed = r[gpu::RB_DEPTH_CLEAR];
                VkClearDepthStencilValue value{ float(packed >> 8) / float(0xFFFFFF),
                                                uint32_t(packed & 0xFF) };
                constexpr VkImageAspectFlags kBoth =
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                VkImageSubresourceRange range{ kBoth, 0, 1, 0, 1 };
                RecordBarrier(target->image, kBoth,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                              kDepthStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
                const VkImage image = target->image;
                Record([=](VkCommandBuffer command) {
                    vkCmdClearDepthStencilImage(command, image,
                                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                &value, 1, &range);
                });
                RecordBarrier(target->image, kBoth,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                              VK_ACCESS_TRANSFER_WRITE_BIT, kDepthAccess,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, kDepthStages);
                target->depthEmpty = packed == 0;
            }
        }
    }

    // One-shot, on the set-up command buffer: a new target leaves UNDEFINED
    // and is cleared before any frame's render pass loads it.
    void ClearNewTarget(Target& target)
    {
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCommandBuffer cmd = g.setup;
        if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return;

        const VkImageAspectFlags aspect = target.depth
            ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
            : VK_IMAGE_ASPECT_COLOR_BIT;
        Barrier(cmd, target.image, aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageSubresourceRange range{ aspect, 0, 1, 0, 1 };
        if (target.depth)
        {
            // Zero, not one: a level tests GEQUAL, and RB_DEPTH_CLEAR is zero.
            VkClearDepthStencilValue value{ 0.0f, 0 };
            vkCmdClearDepthStencilImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        &value, 1, &range);
            target.depthEmpty = true;
        }
        else
        {
            // Not black: a target the title has not drawn into, and a frame that reached
            // the screen without a draw, are then distinguishable from an empty resolve.
            VkClearColorValue value{};
            value.float32[0] = 0.06f; value.float32[1] = 0.07f;
            value.float32[2] = 0.10f; value.float32[3] = 1.0f;
            vkCmdClearColorImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &value, 1, &range);
        }
        Barrier(cmd, target.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                target.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                             : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                target.depth ? kDepthAccess : kColourAccess,
                VK_PIPELINE_STAGE_TRANSFER_BIT, target.depth ? kDepthStages : kColourStages);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        std::lock_guard lock(vk::pipeline::QueueMutex());
        vkResetFences(g.device, 1, &g.setupFence);
        vkQueueSubmit(g.queue, 1, &submit, g.setupFence);
        VkResult res = vkWaitForFences(g.device, 1, &g.setupFence, VK_TRUE, 2000000000ull);
        if (res == VK_TIMEOUT)
        {
            LOGW("renderer: waiting for the set-up fence took >2 seconds; continuing wait...");
            res = vkWaitForFences(g.device, 1, &g.setupFence, VK_TRUE, UINT64_MAX);
        }
        vk::pipeline::Failed(res, "the set-up fence");
    }
}

#endif  // MW2_HAVE_VULKAN
