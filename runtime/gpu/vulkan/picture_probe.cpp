#include "picture_probe.h"

#include <atomic>

namespace
{
    std::atomic<uint64_t> g_last{ 0 };

    uint64_t Pack(const vk::probe::Picture& p)
    {
        return uint64_t(p.serial) | (uint64_t(p.flat) << 32) | (uint64_t(p.isolated) << 33) | (uint64_t(p.luma) << 44);
    }
}

vk::probe::Picture vk::probe::Last()
{
    const uint64_t packed = g_last.load(std::memory_order_relaxed);
    Picture p;
    p.serial = uint32_t(packed);
    p.flat = (packed >> 32) & 1;
    p.isolated = uint32_t((packed >> 33) & 0x7FF);
    p.luma = uint32_t((packed >> 44) & 0xFF);
    return p;
}

#ifdef MW2_HAVE_VULKAN
#include "util.h"
#include "../../diagnostics.h"
#include "../../env.h"
#include "../../log.h"

#include <chrono>

namespace
{
    // Twice a second for the run's first three minutes of frames: the app
    // has decided long before (FrameWatch.kt), and a run that is fine pays
    // for 360 copies of 2,304 samples and nothing after.
    constexpr int64_t kEveryMs = 500;
    constexpr uint32_t kLooks = 360;
    constexpr uint32_t kNoSlot = UINT32_MAX;

    struct State
    {
        VkDevice device = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory imageMemory = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
        const uint8_t* mapped = nullptr;
        uint32_t pending = kNoSlot;   // the slot whose commands hold a copy not read yet
        uint32_t looks = 0;
        int64_t lastMs = 0;
        bool failed = false;
    };
    State g;

    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool Make(VkDevice device, VkPhysicalDevice physical)
    {
        g.device = device;
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);

        VkImageCreateInfo image{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = { vk::probe::kWidth, vk::probe::kHeight, 1 };
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (vkCreateImage(device, &image, nullptr, &g.image) != VK_SUCCESS) return false;
        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(device, g.image, &needs);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = vk::util::FindMemory(memory, needs.memoryTypeBits, 0);
        if (allocate.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(device, &allocate, nullptr, &g.imageMemory) != VK_SUCCESS ||
            vkBindImageMemory(device, g.image, g.imageMemory, 0) != VK_SUCCESS)
            return false;

        VkBufferCreateInfo buffer{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer.size = vk::probe::kWidth * vk::probe::kHeight * 4;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(device, &buffer, nullptr, &g.buffer) != VK_SUCCESS) return false;
        vkGetBufferMemoryRequirements(device, g.buffer, &needs);
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = vk::util::FindMemory(
            memory, needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void* mapped = nullptr;
        if (allocate.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(device, &allocate, nullptr, &g.bufferMemory) != VK_SUCCESS ||
            vkBindBufferMemory(device, g.buffer, g.bufferMemory, 0) != VK_SUCCESS ||
            vkMapMemory(device, g.bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
            return false;
        g.mapped = static_cast<const uint8_t*>(mapped);
        return true;
    }
}

void vk::probe::Record(void* deviceHandle, void* physical, void* commandBuffer, uint32_t slot,
                       void* frame, uint32_t width, uint32_t height)
{
    if (g.failed || g.looks >= kLooks || g.pending != kNoSlot || !frame || !width || !height) return;
    const int64_t now = NowMs();
    if (g.looks && now - g.lastMs < kEveryMs) return;
    if (!g.looks && env::Text("MW2_PICTURE_PROBE") && !env::Flag("MW2_PICTURE_PROBE"))
    {
        g.failed = true;
        return;
    }
    const VkDevice device = static_cast<VkDevice>(deviceHandle);
    if (!g.image && !Make(device, static_cast<VkPhysicalDevice>(physical)))
    {
        g.failed = true;
        LOGW("driver: the first frames cannot be looked at on this device (the copy could not be made)");
        return;
    }
    const VkCommandBuffer commands = static_cast<VkCommandBuffer>(commandBuffer);
    vk::util::Barrier(commands, g.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // Nearest: samples of the frame, not averages of it, which would make noise grey.
    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = { int32_t(width), int32_t(height), 1 };
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = { int32_t(kWidth), int32_t(kHeight), 1 };
    vkCmdBlitImage(commands, static_cast<VkImage>(frame), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   g.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    vk::util::Barrier(commands, g.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { kWidth, kHeight, 1 };
    vkCmdCopyImageToBuffer(commands, g.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.buffer, 1, &copy);
    g.pending = slot;
    g.lastMs = now;
}

void vk::probe::Collect(uint32_t slot)
{
    if (g.pending != slot || !g.mapped) return;
    g.pending = kNoSlot;
    // MW2_TEST_PICTURE=flat or noise, in a build with diagnostics: the look
    // is of one colour or of noise whatever was drawn, to see what the
    // player of a driver that draws that sees on one that draws the game.
    if (const char* test = diag::Text("MW2_TEST_PICTURE"))
    {
        static uint8_t made[kWidth * kHeight * 4];
        uint32_t state = 12345 + g.looks;
        for (uint8_t& byte : made)
        {
            state = state * 1664525u + 1013904223u;
            byte = test[0] == 'n' ? uint8_t(state >> 24) : 0;
        }
        Picture pretended = Analyse(made);
        pretended.serial = ++g.looks;
        g_last.store(Pack(pretended), std::memory_order_relaxed);
        if (pretended.serial == 1) LOGW("driver: MW2_TEST_PICTURE=%s: every frame is reported as that", test);
        return;
    }
    Picture picture = Analyse(g.mapped);
    picture.serial = ++g.looks;
    g_last.store(Pack(picture), std::memory_order_relaxed);
    // The first, and any that is one colour or noise: what a log of a black screen needs.
    if (picture.serial == 1 || picture.flat || picture.isolated >= 120)
    {
        static uint32_t said = 0;
        if (said++ < 12)
            LOGI("driver: frame look %u: %s, %u per thousand samples unlike their neighbours, brightness %u"
                 " (first sample #%02X%02X%02X)", picture.serial, picture.flat ? "one colour" : "a picture",
                 picture.isolated, picture.luma, g.mapped[0], g.mapped[1], g.mapped[2]);
    }
}

void vk::probe::Destroy()
{
    if (!g.device) return;
    if (g.buffer) vkDestroyBuffer(g.device, g.buffer, nullptr);
    if (g.bufferMemory) vkFreeMemory(g.device, g.bufferMemory, nullptr);
    if (g.image) vkDestroyImage(g.device, g.image, nullptr);
    if (g.imageMemory) vkFreeMemory(g.device, g.imageMemory, nullptr);
    g = State{};
}
#endif
