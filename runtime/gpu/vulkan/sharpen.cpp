#include "sharpen.h"

#ifdef MW2_HAVE_VULKAN
#include "util.h"
#include "../../env.h"
#include "../../log.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <mutex>
#include <vector>

namespace
{
#include "shaders/sharpen_shaders.inc"

    // At least the presenter's frames in flight: each has its own descriptor set.
    constexpr uint32_t kSlots = 4;

    // shaders/sharpen.frag's push constants.
    struct Push
    {
        float place[4];
        float params[4];
    };

    // The percent asked for. Unread until the first frame, which takes the
    // environment's.
    constexpr uint32_t kUnread = UINT32_MAX;
    std::atomic<uint32_t> g_percent{ kUnread };

    struct View
    {
        VkImage image;
        VkImageView view;
    };

    struct State
    {
        VkDevice device = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet sets[kSlots]{};
        PFN_vkCmdBeginRenderingKHR beginRendering = nullptr;
        PFN_vkCmdEndRenderingKHR endRendering = nullptr;
        std::vector<View> frames, targets;
        uint32_t said = 0;   // the percent the log last named
    };
    State g;
    // Record on the window's thread against ForgetFrames on the renderer's.
    std::mutex g_lock;
    // A format the pipeline could not be made for, not tried again.
    VkFormat g_failed = VK_FORMAT_UNDEFINED;

    bool Check(VkResult result, const char* what)
    {
        if (result == VK_SUCCESS) return true;
        LOGW("vulkan: the frame's sharpening: %s failed (%d)", what, int(result));
        return false;
    }

    uint32_t Percent()
    {
        uint32_t percent = g_percent.load(std::memory_order_relaxed);
        if (percent != kUnread) return percent;
        const uint32_t environment = uint32_t(std::min<uint64_t>(env::Number("MW2_SHARPEN"), 100));
        // One set meanwhile (Set) stands.
        g_percent.compare_exchange_strong(percent, environment, std::memory_order_relaxed);
        return g_percent.load(std::memory_order_relaxed);
    }

    template <size_t N>
    VkShaderModule Module(const uint32_t (&words)[N])
    {
        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        info.codeSize = sizeof words;
        info.pCode = words;
        VkShaderModule module = VK_NULL_HANDLE;
        Check(vkCreateShaderModule(g.device, &info, nullptr, &module), "a shader");
        return module;
    }

    bool MakePipeline()
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo setInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setInfo.bindingCount = 1;
        setInfo.pBindings = &binding;
        if (!Check(vkCreateDescriptorSetLayout(g.device, &setInfo, nullptr, &g.setLayout), "the set layout"))
            return false;

        VkPushConstantRange range{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &g.setLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        if (!Check(vkCreatePipelineLayout(g.device, &layoutInfo, nullptr, &g.layout), "the pipeline layout"))
            return false;

        const VkShaderModule vertex = Module(kSharpenVertex), fragment = Module(kSharpenFragment);
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo input{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                    VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamics;
        VkPipelineRenderingCreateInfoKHR rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &g.format;

        VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = g.layout;
        const bool made = vertex && fragment &&
            Check(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &info, nullptr, &g.pipeline), "the pipeline");
        if (vertex) vkDestroyShaderModule(g.device, vertex, nullptr);
        if (fragment) vkDestroyShaderModule(g.device, fragment, nullptr);
        return made;
    }

    bool MakeDescriptors()
    {
        VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!Check(vkCreateSampler(g.device, &sampler, nullptr, &g.sampler), "the sampler")) return false;

        VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSlots };
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = kSlots;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        if (!Check(vkCreateDescriptorPool(g.device, &poolInfo, nullptr, &g.pool), "the descriptor pool")) return false;
        VkDescriptorSetLayout layouts[kSlots];
        std::fill(std::begin(layouts), std::end(layouts), g.setLayout);
        VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocate.descriptorPool = g.pool;
        allocate.descriptorSetCount = kSlots;
        allocate.pSetLayouts = layouts;
        return Check(vkAllocateDescriptorSets(g.device, &allocate, g.sets), "the descriptor sets");
    }

    void DestroyViews(std::vector<View>& views)
    {
        for (const View& view : views) vkDestroyImageView(g.device, view.view, nullptr);
        views.clear();
    }

    void DestroyAll()
    {
        if (!g.device) return;
        DestroyViews(g.frames);
        DestroyViews(g.targets);
        if (g.pool) vkDestroyDescriptorPool(g.device, g.pool, nullptr);
        if (g.sampler) vkDestroySampler(g.device, g.sampler, nullptr);
        if (g.pipeline) vkDestroyPipeline(g.device, g.pipeline, nullptr);
        if (g.layout) vkDestroyPipelineLayout(g.device, g.layout, nullptr);
        if (g.setLayout) vkDestroyDescriptorSetLayout(g.device, g.setLayout, nullptr);
        g = State{};
    }

    // For the swapchain's format, on the presenter's device.
    bool Create(VkDevice device, VkFormat format)
    {
        if (g.pipeline && g.format == format) return true;
        if (format == g_failed) return false;
        DestroyAll();
        g.device = device;
        g.format = format;
        g.beginRendering = PFN_vkCmdBeginRenderingKHR(vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR"));
        g.endRendering = PFN_vkCmdEndRenderingKHR(vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR"));
        if (!g.beginRendering || !g.endRendering || !MakePipeline() || !MakeDescriptors())
        {
            LOGW("vulkan: the frame is copied to the window unsharpened (MW2_SHARPEN needs dynamic rendering)");
            DestroyAll();
            g_failed = format;
            return false;
        }
        return true;
    }

    VkImageView ViewOf(std::vector<View>& views, VkImage image, VkFormat format)
    {
        for (const View& view : views)
            if (view.image == image) return view.view;
        VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        if (!Check(vkCreateImageView(g.device, &info, nullptr, &view), "a view")) return VK_NULL_HANDLE;
        views.push_back({ image, view });
        return view;
    }
}

void vk::sharpen::Set(uint32_t percent)
{
    g_percent.store(std::min(percent, 100u), std::memory_order_relaxed);
}

bool vk::sharpen::Record(const Draw& draw)
{
    const uint32_t percent = Percent();
    std::lock_guard lock(g_lock);
    if (g.said != percent && (percent || g.device))
    {
        g.said = percent;
        if (percent) LOGI("sharpen: the frame goes to the window sharpened, %u%%", percent);
        else LOGI("sharpen: the frame is copied to the window as it is");
    }
    if (!percent || draw.slot >= kSlots || draw.to.x <= draw.from.x || draw.to.y <= draw.from.y) return false;
    if (!Create(draw.device, draw.targetFormat)) return false;
    const VkImageView frame = ViewOf(g.frames, draw.frame, kFrameFormat);
    const VkImageView target = ViewOf(g.targets, draw.target, draw.targetFormat);
    if (!frame || !target) return false;

    const VkCommandBuffer commands = draw.commands;
    const VkDescriptorSet set = g.sets[draw.slot];
    VkDescriptorImageInfo image{ g.sampler, frame, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);

    vk::util::Barrier(commands, draw.frame, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vk::util::Barrier(commands, draw.target, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkRenderingAttachmentInfoKHR attachment{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
    attachment.imageView = target;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // Black bars around the frame, where the window is another shape.
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfoKHR rendering{ VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
    rendering.renderArea = { { 0, 0 }, draw.extent };
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    g.beginRendering(commands, &rendering);

    const VkRect2D place{ draw.from, { uint32_t(draw.to.x - draw.from.x), uint32_t(draw.to.y - draw.from.y) } };
    const VkViewport viewport{ float(place.offset.x), float(place.offset.y), float(place.extent.width),
                               float(place.extent.height), 0, 1 };
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &place);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, nullptr);
    const Push push{
        { float(place.offset.x), float(place.offset.y), 1.0f / float(place.extent.width),
          1.0f / float(place.extent.height) },
        { float(percent) / 100.0f, 0, 0, 0 },
    };
    vkCmdPushConstants(commands, g.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    vkCmdDraw(commands, 3, 1, 0, 0);
    g.endRendering(commands);

    vk::util::Barrier(commands, draw.target, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vk::util::Barrier(commands, draw.frame, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    return true;
}

void vk::sharpen::ForgetFrames()
{
    std::lock_guard lock(g_lock);
    if (g.device) DestroyViews(g.frames);
}

void vk::sharpen::ForgetTargets()
{
    std::lock_guard lock(g_lock);
    if (g.device) DestroyViews(g.targets);
}

void vk::sharpen::Destroy()
{
    std::lock_guard lock(g_lock);
    DestroyAll();
}
#endif
