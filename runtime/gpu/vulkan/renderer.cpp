// The renderer: the title's draws and resolves, recorded into Vulkan. This
// file holds its state and brings it up and down; renderer_state.h lists the
// parts.
#include "renderer.h"
#include "../../diagnostics.h"

#ifndef MW2_HAVE_VULKAN
bool vk::renderer::Initialise() { return false; }
void vk::renderer::CheckCompiledShaders(const std::string&) {}
bool vk::renderer::CompiledShaders(std::string&, std::string&) { return true; }
void vk::renderer::Shutdown() {}
bool vk::renderer::Ready() { return false; }
void vk::renderer::Draw(const gpu::RegisterFile&, const DrawCall&) {}
void vk::renderer::Resolve(const gpu::RegisterFile&) {}
void vk::renderer::Swap(uint32_t, void (*)(uint32_t, uint64_t)) {}
void vk::renderer::BeginOcclusionQuery() {}
bool vk::renderer::EndOcclusionQuery(uint32_t, uint64_t& samples) { samples = 0; return true; }
void vk::renderer::CollectOcclusionQueries(void (*)(uint32_t, uint64_t)) {}
void vk::renderer::Report() {}
void vk::renderer::FlashSeen(const char*) {}
uint64_t vk::renderer::CompletedSubmission() { return 0; }
void vk::renderer::BeforeStreamWrite(uint32_t, uint32_t) {}
void vk::renderer::BeforeCompletion() {}
void vk::renderer::Idle() {}
void vk::renderer::PipelineWork(uint32_t& asked, uint32_t& built, uint64_t& longestBuildingMs)
{
    asked = built = 0;
    longestBuildingMs = 0;
}
#else

#include "renderer_state.h"
#include "shader_build.h"
#include "shader_marker.h"
#include "display_table.h"
#include "gpu_time.h"
#include "texture_cache.h"
#include "sharpen.h"
#include "../memory_watch.h"
#include "../../guest_memory.h"
#include "../../crash.h"
#include "../../env.h"
#include "../../log.h"
#include "../../report.h"
#include "../../render_size.h"

#include <algorithm>

namespace vk::renderer::detail
{
    State g;
}

namespace
{
    struct Compiled
    {
        bool checked = false, current = true;
        std::string gameFolder, why, device;
    } g_compiled;
}

void vk::renderer::CheckCompiledShaders(const std::string& gameFolder)
{
    g_compiled.checked = true;
    g_compiled.gameFolder = gameFolder;
}

bool vk::renderer::CompiledShaders(std::string& why, std::string& device)
{
    why = g_compiled.why;
    device = g_compiled.device;
    return g_compiled.current;
}

using namespace vk::renderer::detail;

static void RollbackInitialise()
{
    if (!g.device) return;
    // Both do nothing unless Initialise got as far as starting them.
    vk::gpu_time::Stop();
    StopPassTimes();
    vk::sharpen::ForgetFrames();
    for (uint32_t i = 0; i < kPresentImages; i++)
    {
        if (g.present[i]) vkDestroyImage(g.device, g.present[i], nullptr);
        if (g.presentMemory[i]) vkFreeMemory(g.device, g.presentMemory[i], nullptr);
        g.present[i] = VK_NULL_HANDLE;
        g.presentMemory[i] = VK_NULL_HANDLE;
    }
    for (State::Slot& slot : g.slots)
    {
        for (VkBufferView* view : { &slot.arenaTexels, &slot.arenaTexelWords })
        {
            if (*view) vkDestroyBufferView(g.device, *view, nullptr);
            *view = VK_NULL_HANDLE;
        }
        slot.memorySet = VK_NULL_HANDLE;
    }
    if (g.descriptors)
    {
        vkDestroyDescriptorPool(g.device, g.descriptors, nullptr);
        g.descriptors = VK_NULL_HANDLE;
    }

    gpu::watch::Stop();
    g.shadowBase = 0;
    if (g.arenaMapped)
    {
        vkUnmapMemory(g.device, g.arenaMemory);
        g.arenaMapped = nullptr;
    }
    if (g.arena)
    {
        vkDestroyBuffer(g.device, g.arena, nullptr);
        g.arena = VK_NULL_HANDLE;
    }
    if (g.arenaMemory)
    {
        vkFreeMemory(g.device, g.arenaMemory, nullptr);
        g.arenaMemory = VK_NULL_HANDLE;
    }
    g.arenaBytes = 0;

    if (g.setupFence)
    {
        vkDestroyFence(g.device, g.setupFence, nullptr);
        g.setupFence = VK_NULL_HANDLE;
    }
    for (State::Slot& slot : g.slots)
    {
        if (slot.fence)
        {
            vkDestroyFence(g.device, slot.fence, nullptr);
            slot.fence = VK_NULL_HANDLE;
        }
    }
    if (g.guestQueries)
    {
        vkDestroyQueryPool(g.device, g.guestQueries, nullptr);
        g.guestQueries = VK_NULL_HANDLE;
    }
    if (g.setupCommands)
    {
        vkDestroyCommandPool(g.device, g.setupCommands, nullptr);
        g.setupCommands = VK_NULL_HANDLE;
    }
    g.setup = VK_NULL_HANDLE;
    for (State::Slot& slot : g.slots)
    {
        if (slot.commands)
        {
            vkDestroyCommandPool(g.device, slot.commands, nullptr);
            slot.commands = VK_NULL_HANDLE;
        }
        slot = State::Slot{};
    }
    vk::textures::Shutdown();
    g.presentWidth = 1280;
    g.presentHeight = 720;
    g.scale = 1;
    g.depthFormat = VK_FORMAT_UNDEFINED;
    g.command = VK_NULL_HANDLE;
    g.ready = false;
    g.device = VK_NULL_HANDLE;
    g.physical = VK_NULL_HANDLE;
    g.queue = VK_NULL_HANDLE;
}

bool vk::renderer::Initialise()
{
    if (g.ready) return true;
    if (!vk::pipeline::Ready() && !vk::pipeline::Initialise()) return false;

    g.device = static_cast<VkDevice>(vk::pipeline::Device());
    g.physical = static_cast<VkPhysicalDevice>(vk::pipeline::PhysicalDevice());
    g.queue = static_cast<VkQueue>(vk::pipeline::Queue());
    if (!g.device || !g.queue) { RollbackInitialise(); return false; }
    crash::Step("textures", "the texture cache's staging memory");
    if (!vk::textures::Initialise()) { RollbackInitialise(); return false; }

    vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memory);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(g.physical, &properties);
    // A slot's share starts at a multiple of this, so it covers what a texel
    // buffer view's offset must be a multiple of too.
    g.uniformAlignment = uint32_t(std::max<VkDeviceSize>(
        std::max({ properties.limits.minUniformBufferOffsetAlignment,
                   properties.limits.minStorageBufferOffsetAlignment,
                   properties.limits.minTexelBufferOffsetAlignment }), 256));
    // An index names its draw table entry above its low kTableIndexBits
    // (bindings.h), so the largest index the device takes says how many
    // entries a table may have: all of them where it is 2^32 - 1, 256 where
    // it is the 2^24 - 1 a device without full 32-bit indices has.
    g.mostTableEntries = std::min(
        kMostTableEntries,
        (properties.limits.maxDrawIndexedIndexValue >> bindings::kTableIndexBits) + 1);

    // AMD has no D24_UNORM_S8_UINT, so the depth format is whichever 24-bit-ish
    // combined format the device actually has.
    g.depthFormat = vk::shaders::DepthFormat(g.physical);
    if (g.depthFormat == VK_FORMAT_UNDEFINED) { LOGW("renderer: no depth format"); RollbackInitialise(); return false; }
    if (g.depthFormat != VK_FORMAT_D24_UNORM_S8_UINT) vk::pipeline::NoteFallback("depth is D32S8 (no D24S8)");

    // The console's 2x and 4x surfaces are drawn with that many samples unless
    // MW2_NO_MSAA=1. A count is usable when colour, depth and stencil can all
    // be drawn at it; and a multisampled depth target can only be read back
    // through a render pass resolve, which Vulkan 1.2 has core (or
    // VK_KHR_depth_stencil_resolve before that) and has to take the first
    // sample -- the only mode every such device is required to offer.
    {
        const bool off = env::Flag("MW2_NO_MSAA");
        g.sampleCounts = properties.limits.framebufferColorSampleCounts &
                         properties.limits.framebufferDepthSampleCounts &
                         properties.limits.framebufferStencilSampleCounts;
        VkPhysicalDeviceDepthStencilResolveProperties resolve{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES };
        VkPhysicalDeviceProperties2 properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties2.pNext = &resolve;
        const bool modern = properties.apiVersion >= VK_API_VERSION_1_2;
        if (modern || vk::pipeline::DeviceHasExtension(g.physical, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME))
            vkGetPhysicalDeviceProperties2(g.physical, &properties2);
        const bool depthResolve =
            (modern || vk::pipeline::DeviceHasExtension(g.physical, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME)) &&
            (resolve.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
        g.msaa = !off && depthResolve && (g.sampleCounts & ~VkSampleCountFlags(VK_SAMPLE_COUNT_1_BIT));
        g.stencilUnresolved = depthResolve && (resolve.independentResolveNone || resolve.independentResolve);
        if (g.msaa)
            LOGI("renderer: multisampling on: the device draws at%s%s%s%s, the title's 2x and 4x"
                 " surfaces are drawn at %ux and %ux%s",
                 (g.sampleCounts & VK_SAMPLE_COUNT_2_BIT) ? " 2x" : "",
                 (g.sampleCounts & VK_SAMPLE_COUNT_4_BIT) ? " 4x" : "",
                 (g.sampleCounts & VK_SAMPLE_COUNT_8_BIT) ? " 8x" : "",
                 (g.sampleCounts & VK_SAMPLE_COUNT_16_BIT) ? " 16x" : "",
                 unsigned(HostSamples(1)), unsigned(HostSamples(2)),
                 env::Text("MW2_MSAA") ? " (MW2_MSAA)" : "");
        else
        {
            const char* why = off ? "MW2_NO_MSAA"
                : !modern && !vk::pipeline::DeviceHasExtension(g.physical, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) ? "the device is Vulkan 1.1 without the depth resolve extension"
                : !depthResolve ? "no first-sample depth resolve"
                : "the device draws at 1x only";
            LOGI("renderer: multisampling off (%s): the title's 2x and 4x surfaces are drawn at 1x", why);
            // The switch is the player's choice (and Android's default), not the device's.
            if (!off) vk::pipeline::NoteFallback("multisampled surfaces drawn at 1x");
        }
    }

    // MW2_SCALE=2 or 3 draws everything that many times wider and taller. A
    // surface is at most 4096 of the title's pixels either way, and the scale
    // is lowered until that fits an image of the device's. The frame is the
    // title's (render_size.h).
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(g.physical, &properties);
        const uint32_t asked = uint32_t(std::clamp<uint64_t>(env::Number("MW2_SCALE", 1), 1, 3));
        g.scale = vk::shaders::Scale(g.physical, asked);
        const render::Size frame = render::Frame();
        g.presentWidth = frame.width * g.scale;
        g.presentHeight = frame.height * g.scale;
        if (g.scale != asked)
            LOGW("renderer: MW2_SCALE=%u is more than the device's %u-pixel images hold",
                 asked, properties.limits.maxImageDimension2D);
        if (g.scale > 1)
            LOGI("renderer: drawing at %ux the title's sizes, a %ux%u frame", g.scale,
                 g.presentWidth, g.presentHeight);

        // MW2_SHADOW_SCALE=2 to 4 draws the shadow maps that many times wider
        // and taller, and never smaller than the rest (PassScale).
        const uint32_t shadowAsked = uint32_t(std::clamp<uint64_t>(env::Number("MW2_SHADOW_SCALE", 1), 1,
                                                                   vk::shaders::kMaxShadowScale));
        g.shadowScale = vk::shaders::ShadowScale(g.physical, shadowAsked, g.scale);
        if (g.shadowScale < shadowAsked)
            LOGW("renderer: MW2_SHADOW_SCALE=%u is more than the device's %u-pixel images hold",
                 shadowAsked, properties.limits.maxImageDimension2D);
        if (g.shadowScale != g.scale)
            LOGI("renderer: drawing the shadow maps at %ux the title's sizes", g.shadowScale);
    }

    // The launcher compiled every shader for a device, a scale and the
    // game's files; this run draws on the device and at the scale just
    // settled.
    if (g_compiled.checked)
    {
        g_compiled.device = vk::pipeline::DeviceId(g.physical);
        const std::string fingerprint = vk::shaders::Fingerprint(g.physical, g.shadowScale > 1, g_compiled.gameFolder,
                                                                   env::Flag("MW2_CUSTOM_MAPS"));
        g_compiled.current = vk::shaders::Compiled(fingerprint, &g_compiled.why);
        const vk::shaders::Marker marker = vk::shaders::ReadMarker();
        if (g_compiled.current && marker.shaders && !vk::pipeline::CacheLoaded())
        {
            g_compiled.current = false;
            g_compiled.why = "The graphics driver did not take the compiled shaders.";
        }
        if (g_compiled.current)
        {
            LOGI("renderer: every shader of the game is compiled for this device (%u)", marker.shaders);
            report::ShadersCompiled(marker.shaders ? "yes, " + std::to_string(marker.shaders) + " for this device, driver and scale"
                                                   : std::string("none: the device has no pipeline libraries, so each is compiled at its first draw"));
        }
        else
        {
            LOGW("renderer: the game's shaders are not compiled for this run: %s", g_compiled.why.c_str());
            report::ShadersCompiled("no: " + g_compiled.why);
            // What this run would write back is not what the marker describes.
            vk::pipeline::SetCachePath(nullptr);
        }
    }

    VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool.queueFamilyIndex = vk::pipeline::QueueFamily();
    VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    // A pool each, because a slot is reset on its own while the others are busy.
    for (State::Slot& slot : g.slots)
    {
        if (vkCreateCommandPool(g.device, &pool, nullptr, &slot.commands) != VK_SUCCESS)
        {
            RollbackInitialise();
            return false;
        }
        allocate.commandPool = slot.commands;
        allocate.commandBufferCount = kSegmentBuffers;
        slot.buffers.resize(kSegmentBuffers);
        if (vkAllocateCommandBuffers(g.device, &allocate, slot.buffers.data()) != VK_SUCCESS)
        {
            RollbackInitialise();
            return false;
        }
        allocate.commandBufferCount = 1;
    }

    VkCommandPoolCreateInfo setupPool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    setupPool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    setupPool.queueFamilyIndex = vk::pipeline::QueueFamily();
    if (vkCreateCommandPool(g.device, &setupPool, nullptr, &g.setupCommands) != VK_SUCCESS)
    {
        RollbackInitialise();
        return false;
    }
    allocate.commandPool = g.setupCommands;
    if (vkAllocateCommandBuffers(g.device, &allocate, &g.setup) != VK_SUCCESS)
    {
        RollbackInitialise();
        return false;
    }

    VkQueryPoolCreateInfo queries{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    queries.queryType = VK_QUERY_TYPE_OCCLUSION;
    queries.queryCount = kGuestQueryRing;
    if (vkCreateQueryPool(g.device, &queries, nullptr, &g.guestQueries) != VK_SUCCESS)
        g.guestQueries = VK_NULL_HANDLE;
    // The chip's time is what the scene's size follows, and what the frame
    // rate and the sun's shadows do where the run follows the device.
    const bool scene = StartScene();
    vk::gpu_time::Start(g.physical, g.device, g.queue, vk::pipeline::QueueFamily(), scene || adaptive::On());
    StartPassTimes();

    VkFenceCreateInfo fence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (State::Slot& slot : g.slots)
        if (vkCreateFence(g.device, &fence, nullptr, &slot.fence) != VK_SUCCESS)
        {
            RollbackInitialise();
            return false;
        }
    if (vkCreateFence(g.device, &fence, nullptr, &g.setupFence) != VK_SUCCESS)
    {
        RollbackInitialise();
        return false;
    }

    // Constants, vertex data and indices all live in the arena, which is why
    // it carries every usage at once.
    // Ask for the whole thing and settle for what the device will give: on a
    // shared-memory part this is system RAM, and refusing to start because the
    // largest size did not fit would be the wrong answer.
    //
    // Behind the arena, when there is room, the shadow of guest physical
    // memory: vertex data a draw reads from pages the title has not written
    // since they were copied is read there instead of being copied again (see
    // gpu/memory_watch.h). One buffer, so set 2 reaches both and a fetch
    // constant says which by its address alone.
    // Read through env::, not diag::, because the shadow is 512 MB of resident
    // memory and a release build cannot see a diagnostics switch (diag::Flag is
    // a constant false there): on the device whose memory is shortest the
    // switch would have done nothing at all.
    static const bool noShadow = env::Flag("MW2_NO_SHADOW");
    const uint32_t shadowBytes = guest::kPhysicalEnd - guest::kPhysicalBase;
    uint32_t wanted = uint32_t(env::Number("MW2_ARENA_MB", kArenaWanted >> 20)) << 20;
    bool withShadow = !noShadow;
    // Set 2 is bound per frame slot, from the slot's base on, as one storage
    // buffer, and what one descriptor covers has to be within what a storage
    // buffer descriptor may reach. Past it the binding is invalid and a driver
    // may simply cut it short: Adreno did, and with the whole 384 MB arena in
    // one binding the third slot's share lay beyond the cut -- every third
    // frame drew nothing. So each slot's share is held to the reach, and the
    // shadow, which every slot's binding has to cover, only when the whole
    // buffer is within it. The shadow goes first, the arena is halved after.
    VkPhysicalDeviceProperties limits{};
    vkGetPhysicalDeviceProperties(g.physical, &limits);
    const uint64_t reach = limits.limits.maxStorageBufferRange;
    LOGI("renderer: one storage buffer may reach %llu MB", (unsigned long long)(reach >> 20));
    if (withShadow && uint64_t(wanted) + shadowBytes > reach)
    {
        LOGI("renderer: no shadow of guest memory: %u MB of arena and %u MB of shadow are past"
             " what one storage buffer reaches", wanted >> 20, shadowBytes >> 20);
        vk::pipeline::NoteFallback("no shadow of guest memory (one storage buffer does not reach it)");
        withShadow = false;
    }
    if (uint64_t(wanted) > reach * kFrameSlots)
    {
        wanted = uint32_t((reach * kFrameSlots) & ~uint64_t(0xFFFFF));
        vk::pipeline::NoteFallback("the frame arena is held to three storage buffers' reach");
    }
    crash::Step("arena", "allocating the frame arena, with the shadow of guest memory where it fits");
    for (; wanted >= kArenaLeast; withShadow ? (void)(withShadow = false) : (void)(wanted /= 2))
    {
        VkBufferCreateInfo buffer{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer.size = VkDeviceSize(wanted) + (withShadow ? shadowBytes : 0);
        // Said before it is asked for: a phone short of memory may end the
        // process here rather than refuse.
        LOGI("renderer: asking the driver for %llu MB in one buffer (%u MB of arena%s)",
             (unsigned long long)(buffer.size >> 20), wanted >> 20, withShadow ? ", 512 MB of shadow" : "");
        buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (vk::pipeline::VertexTexels() || vk::pipeline::PixelTexels())
            buffer.usage |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
        if (vkCreateBuffer(g.device, &buffer, nullptr, &g.arena) != VK_SUCCESS) continue;
        VkMemoryRequirements needs{};
        vkGetBufferMemoryRequirements(g.device, g.arena, &needs);
        const uint32_t type = FindMemory(needs.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) { LOGW("renderer: no host-visible memory"); RollbackInitialise(); return false; }
        VkMemoryAllocateInfo allocateArena{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocateArena.allocationSize = needs.size;
        allocateArena.memoryTypeIndex = type;
        if (vkAllocateMemory(g.device, &allocateArena, nullptr, &g.arenaMemory) == VK_SUCCESS &&
            vkBindBufferMemory(g.device, g.arena, g.arenaMemory, 0) == VK_SUCCESS &&
            vkMapMemory(g.device, g.arenaMemory, 0, buffer.size, 0,
                        reinterpret_cast<void**>(&g.arenaMapped)) == VK_SUCCESS)
        {
            g.arenaBytes = wanted;
            if (withShadow) g.shadowBase = wanted;
            vk::pipeline::ArenaHasShadow(withShadow);
            LOGI("renderer: vertex data is copied in %s", vk::pipeline::HostOrderVertices()
                     ? "host byte order" : "the guest's byte order, and exchanged by the vertex shaders");
            // The watch runs either way: without the shadow it still tells
            // texture and constant readers which pages are unwritten.
            gpu::watch::Start(withShadow ? g.arenaMapped + wanted : nullptr,
                              withShadow ? shadowBytes : 0);
            break;
        }
        if (g.arenaMemory) vkFreeMemory(g.device, g.arenaMemory, nullptr);
        vkDestroyBuffer(g.device, g.arena, nullptr);
        g.arenaMemory = VK_NULL_HANDLE;
        g.arena = VK_NULL_HANDLE;
    }
    if (!g.arenaBytes) { LOGW("renderer: no frame arena"); RollbackInitialise(); return false; }

    // A slot's share has to hold everything one submission copies -- a frame's
    // is about 50 MB here against the 512 MB asked for, so three shares fit with
    // room to spare. An overflow is reported rather than silently wrapping.
    const uint32_t share = (g.arenaBytes / kFrameSlots) & ~(g.uniformAlignment - 1);
    g.arenaSlotBytes = share;
    for (uint32_t i = 0; i < kFrameSlots; i++) g.slots[i].arenaBase = i * share;
    g.command = g.slots[g.slot].buffers[0];
    g.arenaBase = g.slots[g.slot].arenaBase;
    LOGI("renderer: %u frame slots of %u KB of arena each", kFrameSlots, g.arenaSlotBytes / 1024);
    if (!MergingDraws())
        LOGI("renderer: MW2_NO_MERGE: every draw is a hardware draw of its own");

    // Before any command is recorded: the recorder thread only reads these.
    // On Vulkan 1.3 core devices (Mali, Tensor, etc.), the commands may be exported
    // without the KHR / EXT suffix. Check both.
    const auto command = [](const char* name, const char* fallback = nullptr) {
        PFN_vkVoidFunction ptr = vkGetDeviceProcAddr(g.device, name);
        if (!ptr && fallback)
            ptr = vkGetDeviceProcAddr(g.device, fallback);
        return ptr;
    };
    dispatch.beginRendering = PFN_vkCmdBeginRenderingKHR(command("vkCmdBeginRenderingKHR", "vkCmdBeginRendering"));
    dispatch.endRendering = PFN_vkCmdEndRenderingKHR(command("vkCmdEndRenderingKHR", "vkCmdEndRendering"));
    dispatch.setCullMode = PFN_vkCmdSetCullModeEXT(command("vkCmdSetCullModeEXT", "vkCmdSetCullMode"));
    dispatch.setFrontFace = PFN_vkCmdSetFrontFaceEXT(command("vkCmdSetFrontFaceEXT", "vkCmdSetFrontFace"));
    dispatch.setDepthTestEnable = PFN_vkCmdSetDepthTestEnableEXT(command("vkCmdSetDepthTestEnableEXT", "vkCmdSetDepthTestEnable"));
    dispatch.setDepthWriteEnable = PFN_vkCmdSetDepthWriteEnableEXT(command("vkCmdSetDepthWriteEnableEXT", "vkCmdSetDepthWriteEnable"));
    dispatch.setDepthCompareOp = PFN_vkCmdSetDepthCompareOpEXT(command("vkCmdSetDepthCompareOpEXT", "vkCmdSetDepthCompareOp"));
    dispatch.setStencilTestEnable = PFN_vkCmdSetStencilTestEnableEXT(command("vkCmdSetStencilTestEnableEXT", "vkCmdSetStencilTestEnable"));
    dispatch.setStencilOp = PFN_vkCmdSetStencilOpEXT(command("vkCmdSetStencilOpEXT", "vkCmdSetStencilOp"));
    if ((dispatch.multiDraw = vk::pipeline::MultiDraw()))
    {
        dispatch.drawMulti = PFN_vkCmdDrawMultiEXT(command("vkCmdDrawMultiEXT"));
        if (!dispatch.drawMulti) dispatch.multiDraw = 0;
    }
    if (!dispatch.beginRendering || !dispatch.endRendering || !dispatch.setCullMode ||
        !dispatch.setStencilOp)
    {
        if (!vk::pipeline::LegacyMode())
        {
            LOGW("renderer: the device did not give dynamic rendering or dynamic state commands");
            RollbackInitialise();
            return false;
        }
        LOGI("renderer: running in legacy mode without dynamic rendering / dynamic state commands");
    }

    // The pool's sizes in the order the kinds may be absent from the end:
    // the texels are moved up where there is no uniform block.
    const bool uniformConstants = vk::pipeline::UniformPixelConstants();
    const bool texelViews = vk::pipeline::VertexTexels() || vk::pipeline::PixelTexels();
    VkDescriptorPoolSize sizes[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kFrameSlots },
                                      { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kFrameSlots },
                                      { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2 * kFrameSlots } };
    uint32_t kinds = uniformConstants ? 2 : 1;
    if (texelViews) sizes[kinds++] = sizes[2];
    VkDescriptorPoolCreateInfo descriptors{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    descriptors.maxSets = kFrameSlots;
    descriptors.poolSizeCount = kinds;
    descriptors.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(g.device, &descriptors, nullptr, &g.descriptors) != VK_SUCCESS)
    {
        RollbackInitialise();
        return false;
    }

    VkDescriptorSetLayout memoryLayout =
        static_cast<VkDescriptorSetLayout>(vk::pipeline::SetLayout(bindings::kArenaSet));
    const VkDeviceSize bufferSize = VkDeviceSize(g.arenaBytes) +
                                    (g.shadowBase ? VkDeviceSize(shadowBytes) : 0);
    for (State::Slot& slot : g.slots)
    {
        VkDescriptorSetAllocateInfo allocateSet{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocateSet.descriptorPool = g.descriptors;
        allocateSet.descriptorSetCount = 1;
        allocateSet.pSetLayouts = &memoryLayout;
        if (vkAllocateDescriptorSets(g.device, &allocateSet, &slot.memorySet) != VK_SUCCESS)
        {
            RollbackInitialise();
            return false;
        }
        // The slot's share and, when there is one, the shadow behind the arena,
        // as dwords and as vec4. The base is a multiple of uniformAlignment,
        // which includes the storage-buffer offset alignment.
        VkDescriptorBufferInfo buffer{ g.arena, slot.arenaBase,
                                       g.shadowBase ? bufferSize - slot.arenaBase
                                                    : VkDeviceSize(g.arenaSlotBytes) };
        // A pixel shader's 256 float constants, where the draw's are: the
        // offset is given when the set is bound (bindings::kPixelConstants).
        // A slot's share ends 4 KB short of its memory, so the range is in
        // bounds wherever the constants are.
        VkDescriptorBufferInfo pixelConstants{ g.arena, slot.arenaBase, bindings::kFloatConstants * 16 };
        VkWriteDescriptorSet writes[5]{};
        for (uint32_t i = 0; i < 3; i++)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = slot.memorySet;
            writes[i].descriptorCount = 1;
        }
        for (uint32_t i = 0; i < 2; i++)
        {
            writes[i].dstBinding = i ? bindings::kArenaFloats : bindings::kArenaBinding;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffer;
        }
        writes[2].dstBinding = bindings::kPixelConstants;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[2].pBufferInfo = &pixelConstants;
        uint32_t written = uniformConstants ? 3 : 2;
        // The share again as texels of four floats and of four dwords; the
        // shadow is not for a shader to read that way.
        struct { VkBufferView* view; VkFormat format; uint32_t binding; } const texels[2] = {
            { &slot.arenaTexels, VK_FORMAT_R32G32B32A32_SFLOAT, bindings::kArenaTexels },
            { &slot.arenaTexelWords, VK_FORMAT_R32G32B32A32_UINT, bindings::kArenaTexelWords } };
        for (uint32_t i = 0; texelViews && i < 2; i++)
        {
            VkBufferViewCreateInfo view{ VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
            view.buffer = g.arena;
            view.format = texels[i].format;
            view.offset = slot.arenaBase;
            view.range = g.arenaSlotBytes;
            if (vkCreateBufferView(g.device, &view, nullptr, texels[i].view) != VK_SUCCESS)
            {
                RollbackInitialise();
                return false;
            }
            writes[written] = writes[0];
            writes[written].dstBinding = texels[i].binding;
            writes[written].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            writes[written].pBufferInfo = nullptr;
            writes[written].pTexelBufferView = texels[i].view;
            written++;
        }
        vkUpdateDescriptorSets(g.device, written, writes, 0, nullptr);
    }

    for (uint32_t i = 0; i < kPresentImages; i++)
    {
        Target image;
        image.format = vk::sharpen::kFrameFormat;
        image.width = g.presentWidth / g.scale;     // MakeImage scales
        image.height = g.presentHeight / g.scale;
        image.scale = g.scale;
        // STORAGE so the display's colour table can be applied to it in place,
        // SAMPLED for the window's sharpening (sharpen.h).
        if (!MakeImage(image, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0))
        {
            RollbackInitialise();
            return false;
        }
        g.present[i] = image.image;
        g.presentMemory[i] = image.memory;
    }

    LOGI("renderer: ready on %s, depth %s, %u MB frame arena%s", vk::pipeline::DeviceName(),
         g.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", g.arenaBytes >> 20,
         g.shadowBase ? " and a shadow of guest memory behind it" : ", no shadow of guest memory");
    vk::pipeline::LogDeviceSummary();
    StartPreparing();
    PrewarmFromCache();
    // A lost device ends the run, the way closing the window does.
    vk::pipeline::OnDeviceLost(crash::RequestExit);
    vk::record::Start(MakeGatheredDraws);
    g.ready = true;
    return true;
}

void vk::renderer::Shutdown()
{
    if (!g.device) return;
    g.ready = false;
    // Before anything is destroyed, the display table's pipeline included: the
    // last frames are still on the GPU, and they use it -- and the last
    // commands may still be with the recorder thread.
    vk::record::Stop();
    StopWarmupScheduler();
    vkDeviceWaitIdle(g.device);
    vk::gpu_time::Stop();
    StopPassTimes();
    SceneReport();
    StopPreparing();
    vk::display::Shutdown();
    SaveCachesAtEnd();
    SummariseTargets();
    ClearRenderPassCache();
    g.pipelineCount = g.pipelines.size();
    g.shaderCount = g.shaders.size();
    for (auto& [key, pipeline] : g.pipelines) if (pipeline) vkDestroyPipeline(g.device, pipeline, nullptr);
    for (VkPipeline pipeline : g.replacedPipelines) vkDestroyPipeline(g.device, pipeline, nullptr);
    for (auto& [key, library] : g.inputLibraries) if (library) vkDestroyPipeline(g.device, library, nullptr);
    for (auto& [key, library] : g.outputLibraries) if (library) vkDestroyPipeline(g.device, library, nullptr);
    g.replacedPipelines.clear();
    g.inputLibraries.clear();
    g.outputLibraries.clear();
    for (auto& [hash, shader] : g.shaders)
    {
        if (shader.library) vkDestroyPipeline(g.device, shader.library, nullptr);
        if (shader.module) vkDestroyShaderModule(g.device, shader.module, nullptr);
    }
    for (auto& [key, target] : g.targets) DestroyTarget(target);
    g.pipelines.clear();
    g.lastPipeline = VK_NULL_HANDLE;
    for (State::RecentPipeline& recent : g.recentPipelines) recent = {};
    g.awaited = {};
    g.shaders.clear();
    for (auto& stage : g.lastShader)
        for (auto& last : stage) last = {};
    g.targets.clear();
    g.lastTargets[0] = g.lastTargets[1] = {};
    vk::sharpen::ForgetFrames();
    for (uint32_t i = 0; i < kPresentImages; i++)
    {
        if (g.present[i]) vkDestroyImage(g.device, g.present[i], nullptr);
        if (g.presentMemory[i]) vkFreeMemory(g.device, g.presentMemory[i], nullptr);
        g.present[i] = VK_NULL_HANDLE;
        g.presentMemory[i] = VK_NULL_HANDLE;
    }
    for (auto& [address, ring] : g.resolvedTo)
        for (Resolved& resolved : ring.copies)
        {
            if (resolved.view) vkDestroyImageView(g.device, resolved.view, nullptr);
            if (resolved.gammaView) vkDestroyImageView(g.device, resolved.gammaView, nullptr);
            if (resolved.attachmentView) vkDestroyImageView(g.device, resolved.attachmentView, nullptr);
            DestroyResolveVolume(resolved);
            if (resolved.image) vkDestroyImage(g.device, resolved.image, nullptr);
            if (resolved.memory) vkFreeMemory(g.device, resolved.memory, nullptr);
        }
    g.resolvedTo.clear();
    g.fetchedWidth.clear();
    g.fetchedVolume.clear();
    g.resolveEpoch++;
    if (g.readBack) vkDestroyBuffer(g.device, g.readBack, nullptr);
    if (g.readBackMemory) vkFreeMemory(g.device, g.readBackMemory, nullptr);
    g.readBack = VK_NULL_HANDLE;
    g.readBackMemory = VK_NULL_HANDLE;
    g.readBackMapped = nullptr;
    if (g.luminanceBack) vkDestroyBuffer(g.device, g.luminanceBack, nullptr);
    if (g.luminanceBackMemory) vkFreeMemory(g.device, g.luminanceBackMemory, nullptr);
    g.luminanceBack = VK_NULL_HANDLE;
    g.luminanceBackMemory = VK_NULL_HANDLE;
    g.luminanceBackMapped = nullptr;
    for (State::Slot& slot : g.slots)
        for (VkBufferView view : { slot.arenaTexels, slot.arenaTexelWords })
            if (view) vkDestroyBufferView(g.device, view, nullptr);
    if (g.descriptors) vkDestroyDescriptorPool(g.device, g.descriptors, nullptr);
    gpu::watch::Stop();
    g.shadowBase = 0;
    if (g.arenaMapped) vkUnmapMemory(g.device, g.arenaMemory);
    if (g.arena) vkDestroyBuffer(g.device, g.arena, nullptr);
    if (g.arenaMemory) vkFreeMemory(g.device, g.arenaMemory, nullptr);
    if (g.guestQueries) vkDestroyQueryPool(g.device, g.guestQueries, nullptr);
    for (State::Slot& slot : g.slots)
    {
        if (slot.fence) vkDestroyFence(g.device, slot.fence, nullptr);
        if (slot.commands) vkDestroyCommandPool(g.device, slot.commands, nullptr);
    }
    if (g.setupFence) vkDestroyFence(g.device, g.setupFence, nullptr);

    if (g.setupCommands) vkDestroyCommandPool(g.device, g.setupCommands, nullptr);

    // The handles are gone; the counters stay, because the report is written
    // after the shutdown.
    g.arena = VK_NULL_HANDLE;
    g.arenaMemory = VK_NULL_HANDLE;
    g.arenaMapped = nullptr;
    g.guestQueries = VK_NULL_HANDLE;
    g.descriptors = VK_NULL_HANDLE;
    g.setupFence = VK_NULL_HANDLE;
    g.setupCommands = VK_NULL_HANDLE;
    for (State::Slot& slot : g.slots) slot = State::Slot{};
    g.recording = false;
    g.device = VK_NULL_HANDLE;
}

bool vk::renderer::Ready() { return g.ready; }

void vk::renderer::Idle() { vk::record::Flush(); }

#endif  // MW2_HAVE_VULKAN
