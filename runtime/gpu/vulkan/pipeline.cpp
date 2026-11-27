#include "pipeline.h"
#include "bindings.h"
#include "bc_decoder.h"
#include "../../env.h"
#include "../../log.h"
#include "../../report.h"

#ifndef MW2_HAVE_VULKAN
// No loader, so nothing here can be built. The callers all tolerate a device
// that never comes up, because that is what a machine with no GPU gives them.
bool vk::pipeline::Initialise(void*, void*, void*, uint32_t, void*) { return false; }
void vk::pipeline::Shutdown() {}
void vk::pipeline::SaveCache() {}
bool vk::pipeline::OneCreation() { return true; }
bool vk::pipeline::Ready() { return false; }
const char* vk::pipeline::DeviceName() { return "none"; }
std::string vk::pipeline::DriverName() { return {}; }
std::string vk::pipeline::DriverVersion() { return {}; }
void*    vk::pipeline::Instance() { return nullptr; }
void*    vk::pipeline::Device() { return nullptr; }
void*    vk::pipeline::PhysicalDevice() { return nullptr; }
void*    vk::pipeline::Queue() { return nullptr; }
uint32_t vk::pipeline::QueueFamily() { return 0; }
bool     vk::pipeline::PreciseOcclusionQueries() { return false; }
float    vk::pipeline::MaxAnisotropy() { return 1.0f; }
bool     vk::pipeline::NonSeamlessCubes() { return false; }
uint32_t vk::pipeline::MultiDraw() { return 0; }
uint32_t vk::pipeline::Samplers() { return 0; }
bool     vk::pipeline::PipelineLibraries() { return false; }
bool     vk::pipeline::HasDynamicRendering() { return false; }
bool     vk::pipeline::HasExtendedDynamicState() { return false; }
bool     vk::pipeline::LegacyMode() { return false; }
bool     vk::pipeline::TextureCompressionBC() { return false; }
bool     vk::pipeline::SubgroupPixelRecords() { return false; }
bool     vk::pipeline::UniformPixelConstants() { return false; }
bool     vk::pipeline::HostOrderVertices() { return false; }
bool     vk::pipeline::VertexTexels() { return false; }
bool     vk::pipeline::PixelTexels() { return false; }
void     vk::pipeline::ArenaHasShadow(bool) {}
bool     vk::pipeline::MirrorClampToEdge() { return false; }
void     vk::pipeline::NoteFallback(const char*) {}
void     vk::pipeline::LogDeviceSummary() {}
std::string vk::pipeline::DeviceSummary() { return "no device"; }
const char* vk::pipeline::Refusal() { return ""; }
std::mutex& vk::pipeline::QueueMutex() { static std::mutex m; return m; }
void*    vk::pipeline::SetLayout(uint32_t) { return nullptr; }
void*    vk::pipeline::Layout() { return nullptr; }
int      vk::pipeline::CreateGraphicsPipeline(const void*, void*, int*) { return -1; }
bool vk::pipeline::CreateModule(const uint32_t*, size_t, const char** error)
{
    if (error) *error = "built without Vulkan";
    return false;
}
bool vk::pipeline::DeviceHasExtension(void*, const char*) { return false; }
uint32_t vk::pipeline::GraphicsFamily(void*) { return UINT32_MAX; }
void vk::pipeline::SetCachePath(const char*) {}
bool vk::pipeline::CacheLoaded() { return false; }
void vk::pipeline::SetDevice(const std::string&) {}
std::string vk::pipeline::DeviceId(void*) { return {}; }
void* vk::pipeline::ChooseDevice(void*, const std::string&, uint32_t*) { return nullptr; }
bool vk::pipeline::CreationFeedback() { return false; }
bool vk::pipeline::ShaderStats() { return false; }
uint32_t vk::pipeline::CaptureFlags() { return 0; }
void vk::pipeline::LogShaderStats(void*, uint64_t, uint64_t) {}
bool vk::pipeline::CreateDevice(void*, uint32_t, const char* const*, uint32_t, void**, void**, void*)
{
    return false;
}
bool vk::pipeline::Failed(int vkResult, const char*) { return vkResult != 0; }
void vk::pipeline::OnDeviceLost(void (*)(const char*)) {}
#else

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <vector>
#include <unordered_set>
#ifdef MW2_ANDROID
#include "../../android/android.h"
#endif
#include "../../env.h"

#include <vulkan/vulkan.h>
#include "loader.h"

#if defined(MW2_VULKAN_DYNAMIC)
namespace { bool OpenVulkan() { std::string error; return vk::loader::Open(error); } }
#else
namespace { bool OpenVulkan() { return true; } }
#endif

namespace
{
    namespace bindings = vk::bindings;

    struct State
    {
        // Owned only when this file brought the device up itself; when the presenter
        // supplies one, teardown leaves it alone.
        bool ownsDevice = false;
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t family = 0;
        bool preciseQueries = false;
        float maxAnisotropy = 1.0f;
        bool nonSeamlessCubes = false;
        uint32_t multiDraw = 0;   // the most draws one vkCmdDrawMulti*EXT may make
        uint32_t samplers = 0;    // the most samplers the texture cache may make
        bool pipelineLibraries = false, fastLinking = false;
        bool creationFeedback = false;
        // The driver is given one pipeline creation at a time (CreationLock).
        bool oneCreation = false;
        // A creation can be asked of a cache without being compiled
        // (pipelineCreationCacheControl): what lets the cache be several.
        bool cacheControl = false;
        // MW2_SHADER_STATS=1 on a device that has VK_KHR_pipeline_executable_properties:
        // the driver's own statistics for every pipeline built (LogShaderStats).
        bool shaderStats = false;
        PFN_vkGetPipelineExecutablePropertiesKHR getExecutableProperties = nullptr;
        PFN_vkGetPipelineExecutableStatisticsKHR getExecutableStatistics = nullptr;
        // MW2_SHADER_IR=<dir>, with the statistics: the driver's internal
        // representations of the executables, written there.
        const char* shaderIr = nullptr;
        PFN_vkGetPipelineExecutableInternalRepresentationsKHR getExecutableRepresentations = nullptr;
        bool cacheLoaded = false;
        // How large the driver's cache was when it was last written to its
        // file (SaveCache).
        size_t cacheSavedBytes = 0;
        bool hasDynamicRendering = false;
        bool hasExtendedDynamicState = false;
        bool legacyMode = false;
        bool textureCompressionBC = false;
        bool subgroupPixelRecords = false;
        bool uniformPixelConstants = false;
        bool vertexTexels = false;
        bool pixelTexels = false;
        bool arenaShadow = false;
        // VK_KHR_sampler_mirror_clamp_to_edge is enabled: without it a sampler
        // may not be made with that address mode.
        bool mirrorClamp = false;
        // Whose chip and whose driver this is, and each thing the device did
        // not have and what is done in its place: one line of the log
        // (LogDeviceSummary), which is what a tester of a device nobody here
        // has sends back.
        std::string deviceClass = "no device";
        // The driver's name and what it says of its version (DriverName).
        std::string driverName, driverInfo;
        // What the device was refused for (Refusal), a literal.
        const char* refusal = "";
        std::vector<std::string> fallbacks;
        std::string name = "none";

        VkDescriptorSetLayout sets[3]{};
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipelineCache cache = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties deviceProperties{};
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
        case VK_ERROR_INVALID_SHADER_NV: return "driver rejected the shader";
        case VK_ERROR_INITIALIZATION_FAILED: return "initialisation failed";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "extension not present";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "feature not present";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "incompatible driver";
        default: return "failed";
        }
    }

}

namespace { std::atomic<void (*)(const char*)> g_deviceLost{ nullptr }; }

void vk::pipeline::OnDeviceLost(void (*handler)(const char*)) { g_deviceLost = handler; }

bool vk::pipeline::Failed(int vkResult, const char* where)
{
    if (vkResult == VK_SUCCESS) return false;
    if (vkResult == VK_ERROR_DEVICE_LOST)
    {
        static std::atomic<bool> reported{ false };
        if (!reported.exchange(true))
        {
            LOGE("vulkan: the device was lost at %s -- the driver reset the GPU under this"
                 " run (after a GPU page fault or a hang; `journalctl -k | grep amdgpu` has"
                 " the fault), and nothing submitted from here on will finish. Ending the run.",
                 where);
            if (auto handler = g_deviceLost.load()) handler("the Vulkan device was lost");
        }
    }
    return true;
}

uint32_t vk::pipeline::GraphicsFamily(void* physical)
{
    VkPhysicalDevice device = static_cast<VkPhysicalDevice>(physical);
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &families, nullptr);
    std::vector<VkQueueFamilyProperties> props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &families, props.data());
    for (uint32_t i = 0; i < families; i++)
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) return i;
    return UINT32_MAX;
}

bool vk::pipeline::DeviceHasExtension(void* physical, const char* name)
{
    uint32_t count = 0;
    VkPhysicalDevice device = static_cast<VkPhysicalDevice>(physical);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> offered(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, offered.data());
    for (const auto& e : offered)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

bool vk::pipeline::CreateDevice(void* physicalDevice, uint32_t family,
                                const char* const* extraExtensions, uint32_t extraCount,
                                void** deviceOut, void** queueOut, void* features)
{
    VkPhysicalDevice physical = static_cast<VkPhysicalDevice>(physicalDevice);
    VkPhysicalDeviceProperties physicalProperties{};
    vkGetPhysicalDeviceProperties(physical, &physicalProperties);
    g.deviceProperties = physicalProperties;
    g.physical = physical;

    // The driver's own name for itself (Vulkan 1.2, or VK_KHR_driver_properties
    // before it; empty where it has neither).
    VkPhysicalDeviceDriverProperties driver{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    {
        VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties.pNext = &driver;
        vkGetPhysicalDeviceProperties2(physical, &properties);
    }
    {
        constexpr uint32_t kArm = 0x13B5, kQualcomm = 0x5143, kImagination = 0x1010, kSamsung = 0x144D;
        const uint32_t vendor = physicalProperties.vendorID;
        const char* family = vendor == kArm ? "Arm Mali"
                           : vendor == kQualcomm ? "Qualcomm Adreno"
                           : vendor == kImagination ? "Imagination PowerVR"
                           : vendor == kSamsung ? "Samsung Xclipse"
                           : "another vendor's";
        char text[512];
        std::snprintf(text, sizeof text, "%s (%s, vendor %04X device %08X), driver %s %s (%u.%u.%u), Vulkan %u.%u.%u",
                      family, physicalProperties.deviceName, vendor, physicalProperties.deviceID,
                      driver.driverName[0] ? driver.driverName : "unnamed", driver.driverInfo,
                      VK_VERSION_MAJOR(physicalProperties.driverVersion),
                      VK_VERSION_MINOR(physicalProperties.driverVersion),
                      VK_VERSION_PATCH(physicalProperties.driverVersion),
                      VK_VERSION_MAJOR(physicalProperties.apiVersion),
                      VK_VERSION_MINOR(physicalProperties.apiVersion),
                      VK_VERSION_PATCH(physicalProperties.apiVersion));
        g.deviceClass = text;
        g.fallbacks.clear();
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical, &supported);
    VkPhysicalDeviceFeatures enabled{};
    // Robust buffer access, because the vertex fetch path reads the frame arena
    // at indices the guest's own data computes. One index past the end is,
    // without it, a GPU page fault: the driver retries it until the queue
    // stalls, and enough of them hung the whole machine. With it the read
    // returns zero or something in bounds, and the draw is merely wrong. Xenia
    // asks for the same thing for the same reason.
    enabled.robustBufferAccess = supported.robustBufferAccess;
    if (!supported.robustBufferAccess)
    {
        LOGW("vulkan: the device offers no robust buffer access; an out-of-range vertex"
             " fetch will fault");
        NoteFallback("no robust buffer access");
    }
    // Compressed BC textures (BC1..BC5) used throughout the title. Strict drivers
    // (Mali, etc.) require this feature to be explicitly enabled before creating BC images.
    enabled.textureCompressionBC = supported.textureCompressionBC;
    g.textureCompressionBC = supported.textureCompressionBC;
    // Adreno's compressed-sampling path has a known BC1-with-1-bit-alpha flicker:
    // surfaces sampling as black one frame and lit the next, missing geometry,
    // and vertex garbage in vegetation. The screenshot ban against it is the
    // same picture the Poco X6 Pro testers sent on Snapdragon devices that Mali
    // phones never show. Mali keeps hardware BC; colour ranges, lighting and
    // fog render identically with the hardware path there. Push Snapdragon
    // devices to the software BC decoder, whose results are correct on both.
    // MW2_FORCE_BC_HW=1 puts it back when a driver proves clean.
    // The env flag is also available to users who hit this on another GPU vendor.
    //
    // That driver is Qualcomm's. Turnip keeps its own BC sampling: the
    // decoder was a tenth to a fifth of the ring consumer and most of its
    // long frames on an AYN Thor, where Modern Warfare 3 streams textures in
    // and its pool moves them all match long -- a Seatown match with every
    // pipeline built lost a blank or more 127 times in four minutes
    // decoding and 32 times sampling, the longest frame 569 ms against 154,
    // with 80 MB of textures in place of 300 and the same picture.
    // MW2_ADRENO_BC=sw asks Turnip for the decoder all the same.
    const vk::bc::AdrenoBC adrenoBc = vk::bc::AdrenoMode();
    const bool decodeAsked = driver.driverID != VK_DRIVER_ID_MESA_TURNIP || env::Text("MW2_ADRENO_BC");
    if (physicalProperties.vendorID == 0x5143 && supported.textureCompressionBC && decodeAsked &&
        !env::Flag("MW2_FORCE_BC_HW") && adrenoBc == vk::bc::AdrenoBC::kAll)
    {
        enabled.textureCompressionBC = VK_FALSE;
        g.textureCompressionBC = false;
        LOGW("vulkan: Adreno BC sampling disabled (known black/missing-geometry flicker);"
             " textures decode in software -- MW2_FORCE_BC_HW=1 re-enables hardware BC,"
             " MW2_ADRENO_BC=dxt1 keeps it for everything but the BC1 family");
    }
    else if (physicalProperties.vendorID == 0x5143 && supported.textureCompressionBC &&
             adrenoBc != vk::bc::AdrenoBC::kAll)
    {
        LOGW("vulkan: MW2_ADRENO_BC=%s: the driver's BC sampling is on"
             " (was off as the Adreno workaround); the BC1 family still decodes in software",
             adrenoBc == vk::bc::AdrenoBC::kHardware ? "hw" : "dxt1");
    }
    if (!supported.textureCompressionBC)
        LOGW("vulkan: the device does not report textureCompressionBC support");
    if (!g.textureCompressionBC)
        NoteFallback(supported.textureCompressionBC ? "BC textures decoded in software (the Adreno workaround)"
                                                    : "BC textures decoded in software (the driver samples none)");
    // The title's occlusion queries are a sample count it compares against a
    // threshold, and without this a query is only allowed to answer "some".
    enabled.occlusionQueryPrecise = supported.occlusionQueryPrecise;
    g.preciseQueries = supported.occlusionQueryPrecise;
    if (!g.preciseQueries) NoteFallback("occlusion queries answer some or none");
    // The title asks for anisotropic filtering on nearly every world texture
    // (2:1, once 4:1), and a sampler cannot give it unless the device enables it.
    enabled.samplerAnisotropy = supported.samplerAnisotropy;
    {
        if (supported.samplerAnisotropy) g.maxAnisotropy = physicalProperties.limits.maxSamplerAnisotropy;
        else NoteFallback("no anisotropic filtering");
        // NVIDIA allows 4000 samplers in all, fewer than the set's slots; the
        // backdrop makes one of its own.
        g.samplers = std::min<uint32_t>(bindings::kSamplerCount,
                                        physicalProperties.limits.maxSamplerAllocationCount - 16);
    }

    // The image format list lets a resolve destination carry an sRGB view beside
    // its own without the driver giving up compression; naming it in an image's
    // pNext without enabling it is invalid.
    std::vector<const char*> extensions(extraExtensions, extraExtensions + extraCount);
    if (DeviceHasExtension(physical, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME))
        extensions.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
    // The console's mirror-once address mode. An extension before Vulkan 1.2
    // and one still offered after it, so asking for it by name serves both.
    g.mirrorClamp = DeviceHasExtension(physical, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
    if (g.mirrorClamp) extensions.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
    else NoteFallback("a mirror-once sampler mirrors and repeats");
    // The driver's word on whether a shader came out of the pipeline cache:
    // the bug report counts the ones the launcher's compile pass had not
    // put there.
    g.creationFeedback = DeviceHasExtension(physical, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
    if (g.creationFeedback) extensions.push_back(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
    // MSAA depth surfaces are read back through a render pass resolve. That is
    // core in Vulkan 1.2 but an extension before it, so a 1.1-or-1.0 device
    // has to be asked for it by name -- its presence is what renderer.cpp
    // gates multisampling on.
    {
        if (physicalProperties.apiVersion < VK_API_VERSION_1_2 &&
            DeviceHasExtension(physical, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME))
            extensions.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
        // The depth resolve's own query goes through GetPhysicalDevice*
        // Properties2, and the 1.1 promoted Maintenance1 is where negative
        // viewport heights (the renderer's flipped clip space) live.
        if (physicalProperties.apiVersion < VK_API_VERSION_1_1)
        {
            if (DeviceHasExtension(physical, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
                extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            if (DeviceHasExtension(physical, VK_KHR_MAINTENANCE1_EXTENSION_NAME))
                extensions.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
        }
    }

    // Direct3D 9 filters each cube face on its own, clamped at its edges; a
    // Vulkan cube blends across them unless told not to. Xenia asks for the
    // same thing.
    VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT nonSeamless{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT };
    if (DeviceHasExtension(physical, VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME))
    {
        VkPhysicalDeviceFeatures2 query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        query.pNext = &nonSeamless;
        vkGetPhysicalDeviceFeatures2(physical, &query);
        query.pNext = nullptr;
        if (nonSeamless.nonSeamlessCubeMap)
            extensions.push_back(VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME);
    }
    g.nonSeamlessCubes = nonSeamless.nonSeamlessCubeMap;
    if (!g.nonSeamlessCubes) NoteFallback("cube maps filtered across their edges");

    // Render passes give way to dynamic rendering, and cull mode, winding and
    // the depth and stencil tests become state set per draw: then a shader
    // compiled on its own, as a pipeline library, fits every target and every
    // draw it is used in, and can be compiled when the title loads it rather
    // than at the first draw -- which is what the console does, where a
    // shader is ready the moment it is loaded.
    const bool core13 = physicalProperties.apiVersion >= VK_API_VERSION_1_3;

    const bool dynamicRenderingExtension =
        DeviceHasExtension(physical, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    const bool dynamicStateExtension =
        DeviceHasExtension(physical, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    const bool pipelineLibrariesExtension =
        DeviceHasExtension(physical, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);

    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamicState{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT libraries{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT };

    {
        VkPhysicalDeviceFeatures2 query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        void** tail = &query.pNext;
        if (core13 || dynamicRenderingExtension)
        {
            *tail = &dynamicRendering;
            tail = &dynamicRendering.pNext;
        }
        if (core13 || dynamicStateExtension)
        {
            *tail = &dynamicState;
            tail = &dynamicState.pNext;
        }
        if (pipelineLibrariesExtension)
        {
            *tail = &libraries;
            tail = &libraries.pNext;
        }
        if (query.pNext)
        {
            vkGetPhysicalDeviceFeatures2(physical, &query);
        }
    }

    // A pipeline can be asked of a cache and refused rather than compiled:
    // the pipeline cache is several of the driver's then, and what is found
    // in each is counted (CreateGraphicsPipeline).
    VkPhysicalDevicePipelineCreationCacheControlFeatures cacheControl{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES };
    const bool cacheControlExtension =
        DeviceHasExtension(physical, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
    if (core13 || cacheControlExtension)
    {
        VkPhysicalDeviceFeatures2 query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        query.pNext = &cacheControl;
        vkGetPhysicalDeviceFeatures2(physical, &query);
        cacheControl.pNext = nullptr;
    }
    g.cacheControl = cacheControl.pipelineCreationCacheControl;
    if (!g.cacheControl) NoteFallback("the pipeline cache is one file");
    if (g.cacheControl && cacheControlExtension)
        extensions.push_back(VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);

    const bool hasDynamicRendering =
        dynamicRendering.dynamicRendering && (dynamicRenderingExtension || core13);
    const bool hasDynamicState =
        dynamicState.extendedDynamicState && (dynamicStateExtension || core13);

    const bool forceLegacy = env::Flag("MW2_FORCE_LEGACY_RENDERING");
    g.hasDynamicRendering = hasDynamicRendering && !forceLegacy;
    g.hasExtendedDynamicState = hasDynamicState && !forceLegacy;
    g.legacyMode = !hasDynamicRendering || !hasDynamicState || forceLegacy;

    LOGI("vulkan: device reports Vulkan %u.%u.%u; dynamic rendering %s (extension %s),"
         " extended dynamic state %s (extension %s)",
         VK_VERSION_MAJOR(physicalProperties.apiVersion),
         VK_VERSION_MINOR(physicalProperties.apiVersion),
         VK_VERSION_PATCH(physicalProperties.apiVersion),
         dynamicRendering.dynamicRendering ? "yes" : "no",
         dynamicRenderingExtension ? "yes" : "no",
         dynamicState.extendedDynamicState ? "yes" : "no",
         dynamicStateExtension ? "yes" : "no");

    if (g.legacyMode)
    {
        LOGI("vulkan: legacy Vulkan 1.1 fallback active: classic VkRenderPass and static pipeline states%s",
             forceLegacy ? " (forced by MW2_FORCE_LEGACY_RENDERING)" : "");
        NoteFallback(forceLegacy ? "render passes and static pipeline state (MW2_FORCE_LEGACY_RENDERING)"
                     : !hasDynamicRendering ? "render passes and static pipeline state (no dynamic rendering)"
                                            : "render passes and static pipeline state (no extended dynamic state)");
    }

    if (dynamicRenderingExtension) extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    if (dynamicStateExtension) extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);

    // Every texture in one set that stays bound, written as textures are
    // made, and a draw's record found through the draw table its base vertex
    // names and handed to the pixel stage (bindings.h). One hardware draw is
    // many of the title's, each with its own textures, so the index into the
    // set is not the same across the draw: a vertex shader says so, and a
    // pixel shader runs once for each record its subgroup holds, which takes
    // the subgroup's first invocation, or where the fragment stage has no
    // subgroup ballot says so too (shader::PixelRecord). Vulkan 1.2 has the
    // rest; a 1.1 device on the render pass fallback needs the descriptor
    // indexing extension.
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
    VkPhysicalDeviceShaderDrawParametersFeatures drawParameters{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES };
    {
        const VkPhysicalDeviceProperties& properties = physicalProperties;
        const bool core = properties.apiVersion >= VK_API_VERSION_1_2;
        if (!core && DeviceHasExtension(physical, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
            extensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        indexing.pNext = &drawParameters;
        VkPhysicalDeviceFeatures2 query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        query.pNext = &indexing;
        vkGetPhysicalDeviceFeatures2(physical, &query);

        VkPhysicalDeviceDescriptorIndexingProperties limits{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES };
        VkPhysicalDeviceSubgroupProperties subgroup{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
        limits.pNext = &subgroup;
        VkPhysicalDeviceProperties2 properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties2.pNext = &limits;
        vkGetPhysicalDeviceProperties2(physical, &properties2);

        // A layout with an update-after-bind set is held to these limits for
        // every descriptor in it, set 2's included. A stage's resources are
        // what it can reach other than samplers, which that limit leaves out:
        // set 1's images, set 2's two views of the arena, and for the pixel
        // stage the one colour target the renderer draws into. Set 1's pool,
        // samplers and all, counts against what all such pools may hold.
        constexpr uint32_t kTextures = bindings::kImageCount + bindings::kSamplerCount;
        constexpr uint32_t kArenaViews = 2;
        constexpr uint32_t kStageResources = bindings::kImageCount + kArenaViews + 1;
        const char* missing =
            properties.apiVersion < VK_API_VERSION_1_1 ? "Vulkan 1.1"
            : !indexing.descriptorBindingSampledImageUpdateAfterBind ||
              !indexing.descriptorBindingPartiallyBound ||
              !indexing.descriptorBindingUpdateUnusedWhilePending ? "descriptor indexing"
            : !supported.shaderSampledImageArrayDynamicIndexing ? "dynamically indexed textures"
            : !drawParameters.shaderDrawParameters ? "shader draw parameters"
            : !indexing.shaderSampledImageArrayNonUniformIndexing ? "non-uniformly indexed textures"
            : limits.maxPerStageDescriptorUpdateAfterBindSampledImages < bindings::kImageCount ||
              limits.maxDescriptorSetUpdateAfterBindSampledImages < bindings::kImageCount ||
              limits.maxPerStageDescriptorUpdateAfterBindSamplers < bindings::kSamplerCount ||
              limits.maxDescriptorSetUpdateAfterBindSamplers < bindings::kSamplerCount ||
              limits.maxPerStageDescriptorUpdateAfterBindStorageBuffers < kArenaViews ||
              limits.maxDescriptorSetUpdateAfterBindStorageBuffers < kArenaViews ||
              limits.maxPerStageUpdateAfterBindResources < kStageResources ||
              limits.maxUpdateAfterBindDescriptorsInAllPools < kTextures
                ? "room for every texture in one set"
            : properties.limits.maxVertexOutputComponents < 4 * (bindings::kRecordLocation + 1) ||
              properties.limits.maxFragmentInputComponents < 4 * (bindings::kRecordLocation + 1)
                ? "seventeen interpolators"
            : nullptr;
        // MW2_TEST_REFUSE_DEVICE=1, in a build with diagnostics: the device
        // is refused as one that lacks descriptor indexing is, to see what
        // the player of such a device sees on one that has everything.
        if (!missing && diag::Flag("MW2_TEST_REFUSE_DEVICE")) missing = "descriptor indexing (MW2_TEST_REFUSE_DEVICE)";
        if (missing)
        {
            g.refusal = missing;
            LOGW("vulkan: the device lacks %s, which the renderer needs", missing);
            LOGW("device: %s; refused: it lacks %s", g.deviceClass.c_str(), missing);
            return false;
        }
        // Turnip on the Adreno 650 (Retroid Pocket Flip 2) has the ballot
        // operations (0x67F) but offers subgroups to compute shaders alone
        // (stages 0x20): the stages are what differ, not the operations.
        const bool ballot = (subgroup.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                            (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT);
        // A Mali offers the ballot to fragment shaders, in subgroups of 16,
        // and is given a record a pixel all the same. No Mali has run either
        // form: Arm's own compiler (tools/malioc_shaders.py) takes both, costs
        // the record a pixel no more than the record a subgroup -- before the
        // subgroup's loop runs a second time -- and spills less of it to the
        // stack, and that form is the one a phone has drawn whole levels with
        // (the Adreno 650). MW2_PIXEL_RECORDS=subgroup or pixel says
        // otherwise where the device has the ballot, to compare the two on one.
        constexpr uint32_t kQualcomm = 0x5143, kArm = 0x13B5;
        const char* records = env::Text("MW2_PIXEL_RECORDS");
        g.subgroupPixelRecords = ballot && (records ? std::strcmp(records, "subgroup") == 0
                                                    : properties.vendorID != kArm);
        LOGI("vulkan: subgroups of %u, stages 0x%X, operations 0x%X, size control %s: a pixel shader %s",
             subgroup.subgroupSize, subgroup.supportedStages, subgroup.supportedOperations,
             properties.apiVersion >= VK_API_VERSION_1_3 ||
             DeviceHasExtension(physical, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) ? "yes" : "no",
             g.subgroupPixelRecords
                 ? "runs once for each draw record its subgroup holds"
                 : !ballot ? "reads its own draw record and indexes textures non-uniformly (no subgroup ballot"
                             " in the fragment stage)"
                 : records ? "reads its own draw record and indexes textures non-uniformly (MW2_PIXEL_RECORDS)"
                           : "reads its own draw record and indexes textures non-uniformly (a Mali:"
                             " MW2_PIXEL_RECORDS=subgroup for the other form)");
        // Without the ballot every pixel fetches each float constant it reads
        // from the arena, where its own record says: there the constants are a
        // uniform block instead (shader::PixelConstants). An Adreno is given
        // them with the ballot too, measured: on the AYN Thor (Adreno 740,
        // Turnip) MW2's S.S.D.D. went from 8.7 to 8.3 ms of GPU a frame
        // and its opening from 11.3 to 11.0. A Mali too, by Arm's compiler:
        // a third fewer load and store cycles in the median pixel shader of
        // either game. MW2_PIXEL_UNIFORMS says otherwise, either way.
        g.uniformPixelConstants = env::Text("MW2_PIXEL_UNIFORMS")
            ? env::Flag("MW2_PIXEL_UNIFORMS")
            : !g.subgroupPixelRecords || properties.vendorID == kQualcomm || properties.vendorID == kArm;
        if (g.uniformPixelConstants)
            LOGI("vulkan: a pixel shader's float constants are a uniform block, bound when a draw's differ");
        if (!g.subgroupPixelRecords)
            NoteFallback(!ballot ? "a draw record a pixel (no subgroup ballot in the fragment stage)"
                         : records ? "a draw record a pixel (MW2_PIXEL_RECORDS)"
                                   : "a draw record a pixel (a Mali's default)");
        if (g.uniformPixelConstants) NoteFallback("pixel constants a uniform block");
        // A vertex shader reading the arena as texels (shader::VertexArena).
        // An Adreno 6xx under Turnip reads a storage buffer a dword a fetch
        // -- ir3 has a load of several only from the 7xx on -- and each
        // fetch is three instructions a vertex, so a vec4 as one texel of
        // four floats, and a draw table entry or a vertex's data four dwords
        // a texel, is what a vertex runs. An Adreno 740 under Turnip has the
        // load of several and is faster with the texels all the same: the
        // opening of S.S.D.D. 9.1 ms of GPU a frame without, 7.9 with these
        // and 7.4 with the pixel shaders' too; Team Player's heavy views
        // 13.2, 11.2 and 10.3. So every Adreno under Turnip has both.
        // Nothing else has been measured -- not Qualcomm's own driver for
        // the same chips -- so the rest keep the storage buffer unless
        // MW2_VERTEX_TEXELS=1 says otherwise. A view has to reach a slot's
        // share of the arena, which is at most 512 MB, 32 million texels,
        // and the two count against a stage's sampled images and resources
        // as set 1's images do.
        {
            constexpr uint32_t kTexelViews = 2;
            bool can = properties.limits.maxTexelBufferElements >= (512u << 20) / 16 &&
                       limits.maxPerStageDescriptorUpdateAfterBindSampledImages >= bindings::kImageCount + kTexelViews &&
                       limits.maxDescriptorSetUpdateAfterBindSampledImages >= bindings::kImageCount + kTexelViews &&
                       limits.maxPerStageUpdateAfterBindResources >= kStageResources + kTexelViews;
            for (VkFormat format : { VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT })
            {
                VkFormatProperties texels{};
                vkGetPhysicalDeviceFormatProperties(physical, format, &texels);
                can = can && (texels.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
            }
            const bool turnip = driver.driverID == VK_DRIVER_ID_MESA_TURNIP;
            g.vertexTexels = can && (env::Text("MW2_VERTEX_TEXELS") ? env::Flag("MW2_VERTEX_TEXELS") : turnip);
            if (g.vertexTexels)
                LOGI("vulkan: vertex shaders read the arena as texels: a vec4, a draw table entry, vertex data");
            // The pixel shaders' side of it (shader::PixelArena): a lit
            // surface's read 26 to 44 dwords of the storage buffer a pixel
            // on a 6xx, three for each texture it samples and four for the
            // alpha test, and asked each texture its size.
            g.pixelTexels = can && (env::Text("MW2_PIXEL_TEXELS") ? env::Flag("MW2_PIXEL_TEXELS") : turnip);
            if (g.pixelTexels)
                LOGI("vulkan: pixel shaders read the arena as texels: the alpha test, and a texel of the record"
                     " for each texture");
        }
        enabled.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
        indexing = VkPhysicalDeviceDescriptorIndexingFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
        indexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        indexing.descriptorBindingPartiallyBound = VK_TRUE;
        indexing.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        indexing.pNext = &drawParameters;
        drawParameters.shaderDrawParameters = VK_TRUE;
    }
    // Turnip (Mesa's Adreno driver, the one players import on Snapdragon
    // handhelds) faulted in its shader compiler building graphics pipeline
    // libraries -- v27.0.0-R2 on the AYN Thor, twice, within a minute or two
    // of play -- and ran three minutes of the same campaign without fault
    // building whole pipelines. MW2_FORCE_PIPELINE_LIBRARIES=1 asks for them
    // anyway, to try a newer build.
    const bool turnip = driver.driverID == VK_DRIVER_ID_MESA_TURNIP;
    g.pipelineLibraries =
        !g.legacyMode &&
        (!turnip || env::Flag("MW2_FORCE_PIPELINE_LIBRARIES")) &&
        DeviceHasExtension(physical, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) &&
        DeviceHasExtension(physical, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
        libraries.graphicsPipelineLibrary;
    if (turnip && !g.pipelineLibraries)
        LOGI("vulkan: Turnip: whole pipelines, not libraries (MW2_FORCE_PIPELINE_LIBRARIES=1 to try them)");
    if (!g.pipelineLibraries) NoteFallback("whole pipelines, not libraries");
    if (g.pipelineLibraries)
    {
        extensions.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        extensions.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT linking{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties.pNext = &linking;
        vkGetPhysicalDeviceProperties2(physical, &properties);
        g.fastLinking = linking.graphicsPipelineLibraryFastLinking;
    }
    else libraries.graphicsPipelineLibrary = VK_FALSE;
#ifdef MW2_ANDROID
    // A phone's driver is given one creation at a time unless it is known to
    // take several: Mali's cache deadlocked with two under way. Turnip takes
    // them (CompileThreads in renderer_pipelines.cpp has the measurements),
    // and a first visit to a level, hundreds of pipelines nobody has
    // compiled, is over in a third of the time on three threads.
    // MW2_ONE_PIPELINE_CREATION=1 or 0 says otherwise, either way.
    g.oneCreation = env::Text("MW2_ONE_PIPELINE_CREATION") ? env::Flag("MW2_ONE_PIPELINE_CREATION") : !turnip;
#endif

    void* chain = features;
    if (g.pipelineLibraries)
    {
        libraries.pNext = chain;
        chain = &libraries;
    }
    if (hasDynamicState || dynamicStateExtension || core13)
    {
        dynamicState.pNext = chain;
        chain = &dynamicState;
    }
    if (hasDynamicRendering || dynamicRenderingExtension || core13)
    {
        dynamicRendering.pNext = chain;
        chain = &dynamicRendering;
    }
    if (g.cacheControl)
    {
        cacheControl.pNext = chain;
        chain = &cacheControl;
    }
    // Draws without indices in a row that change nothing between them are
    // made with one call (renderer_draw.cpp); without it each is made on its
    // own.
    VkPhysicalDeviceMultiDrawFeaturesEXT multiDraw{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT };
    g.multiDraw = 0;
    if (DeviceHasExtension(physical, VK_EXT_MULTI_DRAW_EXTENSION_NAME))
    {
        VkPhysicalDeviceFeatures2 query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        query.pNext = &multiDraw;
        vkGetPhysicalDeviceFeatures2(physical, &query);
        VkPhysicalDeviceMultiDrawPropertiesEXT most{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties.pNext = &most;
        vkGetPhysicalDeviceProperties2(physical, &properties);
        if (multiDraw.multiDraw && most.maxMultiDrawCount > 1)
        {
            extensions.push_back(VK_EXT_MULTI_DRAW_EXTENSION_NAME);
            g.multiDraw = most.maxMultiDrawCount;
        }
    }
    if (!g.multiDraw) NoteFallback("no multi-draw");
    multiDraw = VkPhysicalDeviceMultiDrawFeaturesEXT{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT };
    multiDraw.multiDraw = VK_TRUE;
    if (g.multiDraw)
    {
        multiDraw.pNext = chain;
        chain = &multiDraw;
    }
    // Asked for only by MW2_SHADER_STATS=1: capturing statistics makes the
    // driver keep what it would otherwise drop, so a normal run neither
    // enables the extension nor the feature.
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executables{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR };
    g.shaderStats = false;
    if (env::Flag("MW2_SHADER_STATS") &&
        DeviceHasExtension(physical, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME))
    {
        VkPhysicalDeviceFeatures2 query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        query.pNext = &executables;
        vkGetPhysicalDeviceFeatures2(physical, &query);
        if (executables.pipelineExecutableInfo)
        {
            extensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
            g.shaderStats = true;
        }
    }
    executables = VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR };
    executables.pipelineExecutableInfo = VK_TRUE;
    if (g.shaderStats)
    {
        executables.pNext = chain;
        chain = &executables;
    }
    drawParameters.pNext = chain;
    chain = &indexing;

    VkDeviceCreateInfo info{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    info.pNext = chain;
    if (g.nonSeamlessCubes) { nonSeamless.pNext = chain; info.pNext = &nonSeamless; }
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.pEnabledFeatures = &enabled;
    VkDevice device = VK_NULL_HANDLE;
    const VkResult r = vkCreateDevice(physical, &info, nullptr, &device);
    if (r != VK_SUCCESS) { LOGW("vulkan: no device (%s)", Explain(r)); return false; }
#if defined(MW2_VULKAN_DYNAMIC)
    // From here on the device's entry points come from the driver itself
    // rather than through the loader's dispatch (loader.h).
    vk::loader::LoadDevice(device);
#endif
    if (g.shaderStats)
    {
        g.getExecutableProperties = PFN_vkGetPipelineExecutablePropertiesKHR(
            vkGetDeviceProcAddr(device, "vkGetPipelineExecutablePropertiesKHR"));
        g.getExecutableStatistics = PFN_vkGetPipelineExecutableStatisticsKHR(
            vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"));
        g.getExecutableRepresentations = PFN_vkGetPipelineExecutableInternalRepresentationsKHR(
            vkGetDeviceProcAddr(device, "vkGetPipelineExecutableInternalRepresentationsKHR"));
        g.shaderIr = g.getExecutableRepresentations ? env::Text("MW2_SHADER_IR") : nullptr;
        if (!g.getExecutableProperties || !g.getExecutableStatistics) g.shaderStats = false;
        else LOGI("vulkan: MW2_SHADER_STATS: the driver's statistics for every pipeline built%s%s",
                  g.shaderIr ? ", and what it compiled each vertex shader to in " : "", g.shaderIr ? g.shaderIr : "");
    }
    VkQueue made = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &made);
    *deviceOut = device;
    *queueOut = made;
    return true;
}

namespace
{
    std::string g_wantedDevice;

    // Enough of a device to compile shaders on. No surface, no swapchain: the
    // point is to reach the driver's compiler, which needs neither.
    bool BringUpHeadless()
    {
        if (!OpenVulkan()) { LOGW("pipeline: no Vulkan library"); return false; }

        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "mw2recomp";
        // 1.0 is the floor: negative viewport heights (VK_KHR_maintenance1
        // before that) and still-valid depth resolves (VK_KHR_get_*2 and
        // VK_KHR_depth_stencil_resolve) are all extensions, and the top of the
        // device's version is checked in CreateDevice regardless. The highest
        // the loader will admit to, capped at 1.3: dynamic rendering and
        // extended dynamic state are core there, and a driver that has them
        // in core rather than as extensions can only be asked for them by an
        // application that says it targets 1.3.
        app.apiVersion = VK_API_VERSION_1_0;
#if defined(MW2_VULKAN_DYNAMIC)
        if (vkEnumerateInstanceVersion)
#endif
        {
            uint32_t available = 0;
            if (vkEnumerateInstanceVersion(&available) == VK_SUCCESS)
            {
                if (available >= VK_API_VERSION_1_3)
                    app.apiVersion = VK_API_VERSION_1_3;
                else if (available >= VK_API_VERSION_1_2)
                    app.apiVersion = VK_API_VERSION_1_2;
                else if (available >= VK_API_VERSION_1_1)
                    app.apiVersion = VK_API_VERSION_1_1;
            }
        }

        // A 1.0 instance has to name what 1.1 and 1.2 make core, because the
        // runtime asks for negative viewport heights (maintenance1) and for
        // the pNext-chain properties2 the depth resolve read goes through.
        std::vector<const char*> instanceExtensions;
        if (app.apiVersion < VK_API_VERSION_1_1)
        {
            uint32_t offered = 0;
            if (vkEnumerateInstanceExtensionProperties(nullptr, &offered, nullptr) == VK_SUCCESS && offered)
            {
                std::vector<VkExtensionProperties> list(offered);
                if (vkEnumerateInstanceExtensionProperties(nullptr, &offered, list.data()) == VK_SUCCESS)
                {
                    const auto have = [&list](const char* name) {
                        for (const auto& e : list)
                            if (std::strcmp(e.extensionName, name) == 0) return true;
                        return false;
                    };
                    if (have(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
                        instanceExtensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
                    if (have(VK_KHR_MAINTENANCE1_EXTENSION_NAME))
                        instanceExtensions.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
                }
            }
        }

        VkInstanceCreateInfo info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = uint32_t(instanceExtensions.size());
        info.ppEnabledExtensionNames = instanceExtensions.data();
        VkResult r = vkCreateInstance(&info, nullptr, &g.instance);
        if (r != VK_SUCCESS) { LOGW("pipeline: no Vulkan instance (%s)", Explain(r)); return false; }
#if defined(MW2_VULKAN_DYNAMIC)
        vk::loader::LoadInstance(g.instance);
#endif

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(g.instance, &count, nullptr);
        if (!count) { LOGW("pipeline: no Vulkan device"); return false; }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(g.instance, &count, devices.data());

        uint32_t family = UINT32_MAX;
        g.physical = static_cast<VkPhysicalDevice>(vk::pipeline::ChooseDevice(g.instance, g_wantedDevice, &family));
        if (!g.physical)
        {
            if (g_wantedDevice.empty()) LOGW("pipeline: no graphics queue");
            else LOGW("pipeline: no device %s", g_wantedDevice.c_str());
            return false;
        }

        void* device = nullptr;
        void* queue = nullptr;
        if (!vk::pipeline::CreateDevice(g.physical, family, nullptr, 0, &device, &queue))
            return false;
        g.device = static_cast<VkDevice>(device);
        g.queue = static_cast<VkQueue>(queue);
        g.family = family;
        g.ownsDevice = true;
        return true;
    }

    bool CreateSetLayouts()
    {
        const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        // Every texture and sampler the cache holds (bindings.h). Written while
        // command buffers that bind the set are being recorded and run, each
        // entry before the first draw that names it and never while one that
        // names it is pending; most entries are empty.
        VkDescriptorSetLayoutBinding textures[2]{};
        VkDescriptorBindingFlags textureFlags[2]{};
        for (uint32_t i = 0; i < 2; i++)
        {
            textures[i].binding = i;
            textures[i].descriptorType = i == bindings::kSamplers ? VK_DESCRIPTOR_TYPE_SAMPLER
                                                                 : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            textures[i].descriptorCount = i == bindings::kSamplers ? bindings::kSamplerCount
                                                                  : bindings::kImageCount;
            textures[i].stageFlags = both;
            textureFlags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                              VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
                              VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo textureBindingFlags{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        textureBindingFlags.bindingCount = 2;
        textureBindingFlags.pBindingFlags = textureFlags;

        VkDescriptorSetLayoutBinding memory[5]{};
        for (uint32_t i = 0; i < 2; i++)
        {
            memory[i].binding = i ? bindings::kArenaFloats : bindings::kArenaBinding;
            memory[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            memory[i].descriptorCount = 1;
            memory[i].stageFlags = both;
        }
        memory[2].binding = bindings::kPixelConstants;
        memory[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        memory[2].descriptorCount = 1;
        memory[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        struct Set { const VkDescriptorSetLayoutBinding* bindings; uint32_t count; };
        Set sets[3];
        sets[bindings::kConstantSet] = { nullptr, 0 };    // the constants are in the arena
        sets[bindings::kTextureSet] = { textures, 2 };
        // The uniform block is there or not, and the two texel views after
        // it, for the stages that read them.
        uint32_t memoryBindings = g.uniformPixelConstants ? 3 : 2;
        for (uint32_t binding : { bindings::kArenaTexels, bindings::kArenaTexelWords })
        {
            if (!g.vertexTexels && !g.pixelTexels) break;
            memory[memoryBindings].binding = binding;
            memory[memoryBindings].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            memory[memoryBindings].descriptorCount = 1;
            memory[memoryBindings].stageFlags = (g.vertexTexels ? VK_SHADER_STAGE_VERTEX_BIT : 0u) |
                                                (g.pixelTexels ? VK_SHADER_STAGE_FRAGMENT_BIT : 0u);
            memoryBindings++;
        }
        sets[bindings::kArenaSet] = { memory, memoryBindings };
        for (uint32_t i = 0; i < 3; i++)
        {
            VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            info.bindingCount = sets[i].count;
            info.pBindings = sets[i].bindings;
            if (i == bindings::kTextureSet)
            {
                info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
                info.pNext = &textureBindingFlags;
            }
            VkResult r = vkCreateDescriptorSetLayout(g.device, &info, nullptr, &g.sets[i]);
            if (r != VK_SUCCESS)
            {
                LOGW("pipeline: descriptor set layout %u failed (%s)", i, Explain(r));
                return false;
            }
        }

        // No push constants: the register state a draw needs is in its record
        // (bindings.h).
        VkPipelineLayoutCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        info.setLayoutCount = 3;
        info.pSetLayouts = g.sets;
        VkResult r = vkCreatePipelineLayout(g.device, &info, nullptr, &g.layout);
        if (r != VK_SUCCESS) { LOGW("pipeline: layout failed (%s)", Explain(r)); return false; }
        return true;
    }

    // MW2_PIPELINE_CACHE=<file> in place of the one the game always keeps.
    // Without a file the cache still works, just only within one run.
    std::string g_cachePath;
    const char* CachePath()
    {
        if (env::Flag("MW2_NO_PIPELINE_CACHE")) return nullptr;
        if (const char* asked = env::Text("MW2_PIPELINE_CACHE")) return asked;
        if (!g_cachePath.empty()) return g_cachePath.c_str();
#ifdef MW2_ANDROID
        // No launcher there to name a file: the app's own cache directory.
        static std::string appCache;
        if (appCache.empty() && !android::GetPaths().cache.empty())
            appCache = android::GetPaths().cache + "/pipeline.vkcache";
        if (!appCache.empty()) return appCache.c_str();
#endif
        return nullptr;
    }

    struct PipelineCacheHeader
    {
        uint32_t magic = 0x32435056; // 'VPC2' (Vulkan Pipeline Cache 2)
        uint32_t version = 1;
        uint32_t vendorID = 0;
        uint32_t deviceID = 0;
        uint32_t driverVersion = 0;
        uint8_t  pipelineCacheUUID[VK_UUID_SIZE]{};
        uint64_t dataSize = 0;
    };

    // Mali's Android driver has historically had bugs with cross-session cache
    // blobs. Elsewhere the cache is always kept: the launcher's compile pass
    // depends on it (launcher/precompile.cpp).
    bool IsMali()
    {
#ifdef MW2_ANDROID
        return g.deviceProperties.vendorID == 0x13B5;
#else
        return false;
#endif
    }

    // Whether this GPU's VkPipelineCache is persisted to disk across sessions.
    // Mali has historically had driver bugs with cross-session cache blobs, so
    // we create and use the cache within a session (which allows ISA reuse
    // between pipelines that share shaders) but do not save it to disk by
    // default. MW2_PERSIST_PIPELINE_CACHE=1 overrides: the test reports of
    // Poco X6 Pro (Mali-G615, driver 1.3.247) show every run recompiling
    // hundreds of pipelines at 300-464 ms as the training level loads, which
    // is what stalls the GPU thread behind its guest waits long enough for
    // the title to error out -- worth a player let-in even against the
    // historical driver bug. If the driver rejects a loaded blob, CreateCache
    // logs it and falls back to an empty cache, so the failure is soft.
    bool PersistCache()
    {
        static const bool asked = env::Flag("MW2_PERSIST_PIPELINE_CACHE");
        if (asked) LOGI("pipeline: persisting the pipeline cache across runs (user request) on %04X", g.deviceProperties.vendorID);
        return !IsMali() || asked;
    }

    // How much of the driver's cache is kept, MW2_PIPELINE_CACHE_MB; 0 is all
    // of it in one file. That is the launcher's, which holds every shader of
    // the game and is whole or of no use. A phone's holds what its player has
    // drawn under every build of the shaders the app has had, and a pipeline
    // an update translates differently is never asked for again: an AYN
    // Thor's file held 5,656 pipelines in 47 MB, 1,571 of them the ones its
    // campaign still asked for. The driver's cache has no way to let one go
    // and nothing in it bounds the file (Mesa's vk_pipeline_cache_load reads
    // as many objects as the blob says, each copied into memory), so it grew
    // until this file's own limit of 64 MB refused it whole, and the next
    // run compiled everything again.
    //
    // So past a budget the cache is several of the driver's. The one being
    // added to is `<path>`; at an eighth of the budget it is closed as
    // `<path>.<n>`, never written again, and an empty one takes its place. A
    // creation is asked of each (CreateGraphicsPipeline) and what is found in
    // a closed one is counted in `<path>.index`, with the session it was
    // found in. At a start the closed ones nothing was found in for
    // kUnusedSessions go, and past the budget the ones least was found in
    // for their size: what they still held of use is compiled once more,
    // into the one being added to.
    constexpr uint32_t kCacheParts = 8;
    constexpr uint32_t kUnusedSessions = 8;
    uint64_t CacheBudget()
    {
#ifdef MW2_ANDROID
        constexpr uint64_t kMegabytes = 64;
#else
        constexpr uint64_t kMegabytes = 0;
#endif
        static const uint64_t bytes = env::Number("MW2_PIPELINE_CACHE_MB", kMegabytes) << 20;
        return g.cacheControl ? bytes : 0;
    }

    struct ClosedCache
    {
        VkPipelineCache cache = VK_NULL_HANDLE;
        uint32_t number = 0;   // of its file, `<path>.<number>`
        uint64_t bytes = 0;
        // The last session a creation was found in it, and how many were
        // then; in this one so far.
        uint32_t lastUsed = 0;
        uint64_t found = 0;
        std::atomic<uint64_t> foundNow{ 0 };
    };
    struct Caches
    {
        // A creation reads the list and the cache being added to; closing
        // that one changes both.
        std::shared_mutex lock;
        std::deque<ClosedCache> closed;
        // Where the last creation was found: a level's pipelines were
        // compiled together, so the next is likely beside it.
        std::atomic<uint32_t> lastFound{ 0 };
        uint32_t session = 1;
        uint32_t lastNumber = 0;
        std::atomic<uint64_t> found{ 0 }, foundClosed{ 0 }, compiled{ 0 };
        // What the index and the log last said.
        uint64_t indexed = 0, logged = 0;
        std::chrono::steady_clock::time_point said{};
    } c;

    // A creation holds the caches' lock shared, so that several run side by
    // side and none of them finds the cache being added to replaced under it
    // (CloseCache); alone where the driver must be given one at a time
    // (g.oneCreation). Elsewhere the fast link at a draw must not wait behind
    // the optimised link in the background.
    class CreationLock
    {
    public:
        explicit CreationLock(std::shared_mutex& lock) : m_lock(lock), m_alone(g.oneCreation)
        {
            if (m_alone) m_lock.lock();
            else m_lock.lock_shared();
        }
        ~CreationLock()
        {
            if (m_alone) m_lock.unlock();
            else m_lock.unlock_shared();
        }
        CreationLock(const CreationLock&) = delete;
        CreationLock& operator=(const CreationLock&) = delete;

    private:
        std::shared_mutex& m_lock;
        const bool m_alone;
    };

    std::string ClosedPath(const char* path, uint32_t number) { return std::string(path) + "." + std::to_string(number); }

    // A cache file's payload, if it was written by this driver for this device.
    std::vector<uint8_t> ReadCacheFile(const std::string& path)
    {
        std::vector<uint8_t> data;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return data;
        std::error_code ec;
        const uint64_t fileSize = std::filesystem::file_size(path, ec);
        PipelineCacheHeader header{};
        if (ec || fileSize <= sizeof header || std::fread(&header, sizeof header, 1, f) != 1)
        {
            if (fileSize) LOGW("pipeline: cache file %s size invalid (%llu bytes), ignoring", path.c_str(),
                               (unsigned long long)fileSize);
        }
        else if (header.magic != PipelineCacheHeader{}.magic || header.version != 1 ||
                 header.vendorID != g.deviceProperties.vendorID || header.deviceID != g.deviceProperties.deviceID ||
                 header.driverVersion != g.deviceProperties.driverVersion ||
                 std::memcmp(header.pipelineCacheUUID, g.deviceProperties.pipelineCacheUUID, VK_UUID_SIZE) != 0 ||
                 header.dataSize != fileSize - sizeof header)
        {
            LOGW("pipeline: cache header in %s invalidated (driver/device update or UUID mismatch), starting fresh",
                 path.c_str());
        }
        else
        {
            data.resize(header.dataSize);
            if (std::fread(data.data(), 1, data.size(), f) != data.size())
            {
                data.clear();
                LOGW("pipeline: truncated cache payload in %s, starting empty", path.c_str());
            }
        }
        std::fclose(f);
        return data;
    }

    // `taken`: the driver made it from `initial`. One it refuses comes back
    // empty.
    VkPipelineCache MakeCache(const std::vector<uint8_t>& initial, bool& taken)
    {
        VkPipelineCacheCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        info.initialDataSize = initial.size();
        info.pInitialData = initial.empty() ? nullptr : initial.data();
        VkPipelineCache cache = VK_NULL_HANDLE;
        taken = false;
        if (vkCreatePipelineCache(g.device, &info, nullptr, &cache) == VK_SUCCESS)
        {
            taken = !initial.empty();
            return cache;
        }
        if (initial.empty()) return VK_NULL_HANDLE;
        LOGW("pipeline: driver rejected pipeline cache data, falling back to empty cache");
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        return vkCreatePipelineCache(g.device, &info, nullptr, &cache) == VK_SUCCESS ? cache : VK_NULL_HANDLE;
    }

    size_t CacheBytes(VkPipelineCache cache)
    {
        size_t size = 0;
        return vkGetPipelineCacheData(g.device, cache, &size, nullptr) == VK_SUCCESS ? size : 0;
    }

    // Beside it first, then in its place: a run that ends halfway through
    // the write leaves the file it had, not a broken one.
    bool ReplaceFile(const std::string& path, const void* first, size_t firstBytes, const void* rest,
                     size_t restBytes)
    {
        const std::string partial = path + ".partial";
        std::FILE* f = std::fopen(partial.c_str(), "wb");
        if (!f) return false;
        const bool written = std::fwrite(first, 1, firstBytes, f) == firstBytes &&
                             (!restBytes || std::fwrite(rest, 1, restBytes, f) == restBytes);
        std::error_code ec;
        if (std::fclose(f) != 0 || !written)
        {
            std::filesystem::remove(partial, ec);
            LOGW("pipeline: failed to write %s", path.c_str());
            return false;
        }
        std::filesystem::rename(partial, path, ec);
        if (ec) LOGW("pipeline: %s could not be put in place (%s)", path.c_str(), ec.message().c_str());
        return !ec;
    }

    struct Used
    {
        uint32_t lastUsed = 0;
        uint64_t found = 0;
    };

    // The session the index was last written in, and what was found in each
    // closed cache up to then.
    uint32_t ReadIndex(const char* path, std::map<uint32_t, Used>& used)
    {
        std::FILE* f = std::fopen((std::string(path) + ".index").c_str(), "r");
        if (!f) return 0;
        unsigned session = 0, number = 0, lastUsed = 0;
        unsigned long long found = 0;
        if (std::fscanf(f, "session %u", &session) == 1)
            while (std::fscanf(f, " closed %u %u %llu", &number, &lastUsed, &found) == 3)
                used[number] = { lastUsed, found };
        std::fclose(f);
        return session;
    }

    void WriteIndex(const char* path)
    {
        std::string text = "session " + std::to_string(c.session) + "\n";
        for (ClosedCache& closed : c.closed)
        {
            if (const uint64_t now = closed.foundNow.load(std::memory_order_relaxed))
            {
                closed.lastUsed = c.session;
                closed.found = now;
            }
            text += "closed " + std::to_string(closed.number) + " " + std::to_string(closed.lastUsed) + " " +
                    std::to_string(closed.found) + "\n";
        }
        if (ReplaceFile(std::string(path) + ".index", text.data(), text.size(), nullptr, 0))
            c.indexed = c.foundClosed.load(std::memory_order_relaxed);
    }

    // The closed caches beside `path`, less the ones that go; `openBytes` is
    // what the one being added to holds.
    void OpenClosedCaches(const char* path, uint64_t openBytes)
    {
        namespace fs = std::filesystem;
        std::map<uint32_t, Used> used;
        c.session = ReadIndex(path, used) + 1;

        struct File
        {
            uint32_t number;
            uint64_t bytes;
            Used used;
        };
        std::vector<File> files;
        uint64_t total = openBytes;
        const fs::path whole(path);
        const std::string prefix = whole.filename().string() + ".";
        std::error_code ec;
        for (const fs::directory_entry& entry :
             fs::directory_iterator(whole.has_parent_path() ? whole.parent_path() : fs::path("."), ec))
        {
            const std::string name = entry.path().filename().string();
            if (name.size() <= prefix.size() || name.size() > prefix.size() + 9 ||
                name.compare(0, prefix.size(), prefix) != 0 ||
                !std::all_of(name.begin() + prefix.size(), name.end(), [](unsigned char ch) { return std::isdigit(ch); }))
                continue;
            const uint32_t number = uint32_t(std::strtoul(name.c_str() + prefix.size(), nullptr, 10));
            const auto known = used.find(number);
            files.push_back({ number, uint64_t(entry.file_size(ec)), known != used.end() ? known->second : Used{} });
            c.lastNumber = std::max(c.lastNumber, number);
            total += files.back().bytes;
        }

        // The first to go first: the longest unused, then the one least was
        // found in for its size.
        std::sort(files.begin(), files.end(), [](const File& a, const File& b) {
            if (a.used.lastUsed != b.used.lastUsed) return a.used.lastUsed < b.used.lastUsed;
            return a.used.found * b.bytes < b.used.found * a.bytes;
        });
        const uint64_t budget = CacheBudget();
        size_t gone = 0;
        for (; gone < files.size(); gone++)
        {
            const File& file = files[gone];
            const bool unused = file.used.lastUsed + kUnusedSessions < c.session;
            if (!unused && (!budget || total <= budget)) break;
            LOGI("pipeline: %s.%u goes (%llu KB, %llu pipelines found in it in session %u of %u): %s", path,
                 file.number, (unsigned long long)(file.bytes / 1024), (unsigned long long)file.used.found,
                 file.used.lastUsed, c.session, unused ? "unused for too long" : "the cache is over its budget");
            fs::remove(ClosedPath(path, file.number), ec);
            total -= file.bytes;
        }
        // Asked in this order: the ones most was found in first.
        for (size_t i = files.size(); i-- > gone;)
        {
            const File& file = files[i];
            const std::string name = ClosedPath(path, file.number);
            const std::vector<uint8_t> data = ReadCacheFile(name);
            bool taken = false;
            const VkPipelineCache cache = data.empty() ? VK_NULL_HANDLE : MakeCache(data, taken);
            if (!taken)
            {
                // Another driver's, or another device's: nothing will be
                // found in it again.
                if (cache) vkDestroyPipelineCache(g.device, cache, nullptr);
                fs::remove(name, ec);
                continue;
            }
            ClosedCache& closed = c.closed.emplace_back();
            closed.cache = cache;
            closed.number = file.number;
            closed.bytes = file.bytes;
            closed.lastUsed = file.used.lastUsed;
            closed.found = file.used.found;
        }
        // At every start, not only once something is found in a closed file:
        // a cache still in its first file was session 1 at every start, and
        // read in the log as a cache lost between runs.
        WriteIndex(path);
    }

    void CreateCache()
    {
        std::unique_lock lock(c.lock);
        if (env::Flag("MW2_NO_PIPELINE_CACHE"))
        {
            LOGI("pipeline: VkPipelineCache bypassed (MW2_NO_PIPELINE_CACHE)");
            return;
        }

        // Mali: skip disk-load by default to avoid cross-session driver
        // instability, but still create an in-session cache for ISA reuse.
        // MW2_PERSIST_PIPELINE_CACHE=1 asks for the disk blob back (see
        // PersistCache above).
        std::vector<uint8_t> initial;
        const bool persist = PersistCache();
        const char* path = persist ? CachePath() : nullptr;
        if (path)
        {
            LOGI("pipeline: checking persistent pipeline cache at: %s", path);
            initial = ReadCacheFile(path);
            if (g.cacheControl) OpenClosedCaches(path, initial.size());
        }
        else if (!persist)
        {
            LOGI("pipeline: Mali GPU detected -- using in-session VkPipelineCache only;"
                 " set MW2_PERSIST_PIPELINE_CACHE=1 to keep the disk blob across runs");
            vk::pipeline::NoteFallback("the pipeline cache is not kept between runs (Mali)");
        }

        bool taken = false;
        g.cache = MakeCache(initial, taken);
        // What it holds is what its file holds: nothing to write until it grows.
        if (taken) g.cacheSavedBytes = CacheBytes(g.cache);
        g.cacheLoaded = taken || !c.closed.empty();
        if (g.cacheLoaded)
        {
            uint64_t closedBytes = 0;
            for (const ClosedCache& closed : c.closed) closedBytes += closed.bytes;
            LOGI("pipeline: loaded %zu KB of pipeline cache for GPU %04X:%04X driver %08X, and %zu closed files of"
                 " %llu KB (session %u, budget %llu MB)",
                 taken ? initial.size() / 1024 : size_t(0), g.deviceProperties.vendorID, g.deviceProperties.deviceID,
                 g.deviceProperties.driverVersion, c.closed.size(), (unsigned long long)(closedBytes / 1024),
                 c.session, (unsigned long long)(CacheBudget() >> 20));
        }
    }

    // The cache being added to has reached its share of the budget: its file
    // becomes a closed one's and an empty cache takes its place.
    void CloseCache(const char* path, size_t size)
    {
        bool taken = false;
        const VkPipelineCache empty = MakeCache({}, taken);
        if (!empty) return;
        const uint32_t number = c.lastNumber + 1;
        std::error_code ec;
        std::filesystem::rename(path, ClosedPath(path, number), ec);
        if (ec)
        {
            LOGW("pipeline: %s could not be closed as its file %u (%s)", path, number, ec.message().c_str());
            vkDestroyPipelineCache(g.device, empty, nullptr);
            return;
        }
        c.lastNumber = number;
        ClosedCache& closed = c.closed.emplace_back();
        closed.cache = g.cache;
        closed.number = number;
        closed.bytes = size + sizeof(PipelineCacheHeader);
        closed.lastUsed = c.session;
        g.cache = empty;
        g.cacheSavedBytes = CacheBytes(empty);
        LOGI("pipeline: closed %zu KB of pipeline cache as %s.%u; %zu closed files now", size / 1024, path, number,
             c.closed.size());
        WriteIndex(path);
    }

    // `last`: the run is ending. The index and the log's line otherwise wait
    // for kSayEvery, since the warm-up saves after every few pipelines.
    constexpr auto kSayEvery = std::chrono::seconds(10);
    void SaveCache(bool last = false)
    {
        std::unique_lock lock(c.lock);
        // Do not persist cache blobs to disk on Mali by default --
        // in-session use only. MW2_PERSIST_PIPELINE_CACHE=1 keeps saving.
        if (!PersistCache()) return;
        const char* path = CachePath();
        if (!path || !g.cache) return;
        const size_t size = CacheBytes(g.cache);
        if (!size) return;
        // Nothing new since it was last written: the driver's cache only
        // grows, and the warm-up asks after every few pipelines whether or
        // not they were in it already -- 147 times in a hundred seconds of a
        // level played before, 10 MB to the phone's storage each time.
        if (size != g.cacheSavedBytes)
        {
            size_t got = size;
            std::vector<uint8_t> data(size);
            if (vkGetPipelineCacheData(g.device, g.cache, &got, data.data()) != VK_SUCCESS) return;

            PipelineCacheHeader header{};
            header.vendorID = g.deviceProperties.vendorID;
            header.deviceID = g.deviceProperties.deviceID;
            header.driverVersion = g.deviceProperties.driverVersion;
            std::memcpy(header.pipelineCacheUUID, g.deviceProperties.pipelineCacheUUID, VK_UUID_SIZE);
            header.dataSize = got;
            if (!ReplaceFile(path, &header, sizeof header, data.data(), got)) return;
            g.cacheSavedBytes = size;
            LOGI("pipeline: wrote %zu KB of pipeline cache to %s", (got + sizeof header) / 1024, path);
        }

        const uint64_t budget = CacheBudget();
        const auto now = std::chrono::steady_clock::now();
        const bool closes = budget && size >= budget / kCacheParts;
        if (closes) CloseCache(path, size);
        else if (!last && now - c.said < kSayEvery) return;
        else if (!c.closed.empty() && c.indexed != c.foundClosed.load(std::memory_order_relaxed)) WriteIndex(path);
        c.said = now;

        const uint64_t found = c.found.load(std::memory_order_relaxed);
        const uint64_t compiled = c.compiled.load(std::memory_order_relaxed);
        if (found + compiled != c.logged)
        {
            c.logged = found + compiled;
            uint64_t bytes = g.cacheSavedBytes;
            for (const ClosedCache& closed : c.closed) bytes += closed.bytes;
            LOGI("pipeline: the cache is %llu KB in %zu files; %llu pipelines found in it so far (%llu in the closed"
                 " files), %llu compiled by the driver",
                 (unsigned long long)(bytes / 1024), c.closed.size() + 1, (unsigned long long)found,
                 (unsigned long long)c.foundClosed.load(std::memory_order_relaxed), (unsigned long long)compiled);
        }
    }

    VkShaderModule MakeModule(const uint32_t* spirv, size_t words, VkResult& result)
    {
        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        info.codeSize = words * 4;
        info.pCode = spirv;
        VkShaderModule module = VK_NULL_HANDLE;
        result = vkCreateShaderModule(g.device, &info, nullptr, &module);
        return module;
    }
}

void vk::pipeline::SetCachePath(const char* path) { g_cachePath = path ? path : ""; }
bool vk::pipeline::CacheLoaded() { return g.cacheLoaded; }
void vk::pipeline::SetDevice(const std::string& id) { g_wantedDevice = id; }
bool vk::pipeline::CreationFeedback() { return g.creationFeedback; }
bool vk::pipeline::ShaderStats() { return g.shaderStats; }
uint32_t vk::pipeline::CaptureFlags()
{
    if (!g.shaderStats) return 0;
    return VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
           (g.shaderIr ? VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR : 0);
}

void vk::pipeline::LogShaderStats(void* pipeline, uint64_t vertexHash, uint64_t pixelHash)
{
    if (!g.shaderStats || !pipeline || !g.device) return;
    const VkPipeline handle = static_cast<VkPipeline>(pipeline);
    VkPipelineInfoKHR pipelineInfo{ VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR };
    pipelineInfo.pipeline = handle;
    uint32_t count = 0;
    if (g.getExecutableProperties(g.device, &pipelineInfo, &count, nullptr) != VK_SUCCESS || !count) return;
    const VkPipelineExecutablePropertiesKHR emptyProperties{ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR };
    std::vector<VkPipelineExecutablePropertiesKHR> executables(count, emptyProperties);
    if (g.getExecutableProperties(g.device, &pipelineInfo, &count, executables.data()) < 0) return;
    // A name is one token on the line, so a log reader can split on spaces.
    const auto token = [](std::string& out, const char* text) {
        for (; *text; ++text) out += (*text == ' ') ? '_' : *text;
    };
    for (uint32_t i = 0; i < count; ++i)
    {
        VkPipelineExecutableInfoKHR which{ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR };
        which.pipeline = handle;
        which.executableIndex = i;
        uint32_t statCount = 0;
        if (g.getExecutableStatistics(g.device, &which, &statCount, nullptr) != VK_SUCCESS || !statCount) continue;
        const VkPipelineExecutableStatisticKHR emptyStatistic{ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR };
        std::vector<VkPipelineExecutableStatisticKHR> statistics(statCount, emptyStatistic);
        if (g.getExecutableStatistics(g.device, &which, &statCount, statistics.data()) < 0) continue;

        // Turnip has a binning vertex shader beside the vertex shader, both of
        // the vertex stage: the executable's own name tells them apart.
        const VkShaderStageFlags stages = executables[i].stages;
        std::string stage = stages == VK_SHADER_STAGE_VERTEX_BIT ? "vertex"
                          : stages == VK_SHADER_STAGE_FRAGMENT_BIT ? "fragment" : "stage" + std::to_string(stages);
        stage += '/';
        token(stage, executables[i].name);
        std::string line;
        char value[40];
        for (uint32_t s = 0; s < statCount; ++s)
        {
            const VkPipelineExecutableStatisticKHR& statistic = statistics[s];
            switch (statistic.format)
            {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                std::snprintf(value, sizeof value, "%u", unsigned(statistic.value.b32)); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                std::snprintf(value, sizeof value, "%lld", (long long)statistic.value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                std::snprintf(value, sizeof value, "%llu", (unsigned long long)statistic.value.u64); break;
            default:
                std::snprintf(value, sizeof value, "%g", statistic.value.f64); break;
            }
            line += ' ';
            token(line, statistic.name);
            line += '=';
            line += value;
        }
        LOGI("shaderstats: vs %016llx ps %016llx stage %s:%s", (unsigned long long)vertexHash,
             (unsigned long long)pixelHash, stage.c_str(), line.c_str());
    }
    // MW2_SHADER_IR=<dir>: what the driver compiled each executable to, as it
    // offers it (Turnip: the NIR and the ir3 disassembly, binning variant
    // apart), a file each. Only the first pipeline of a vertex shader: the
    // question is what a vertex costs, and one pixel shader's linking answers it.
    if (!g.shaderIr) return;
    {
        static std::mutex lock;
        static std::unordered_set<uint64_t> written;
        std::lock_guard held(lock);
        if (!written.insert(vertexHash).second) return;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        VkPipelineExecutableInfoKHR which{ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR };
        which.pipeline = handle;
        which.executableIndex = i;
        uint32_t forms = 0;
        if (g.getExecutableRepresentations(g.device, &which, &forms, nullptr) != VK_SUCCESS || !forms) continue;
        const VkPipelineExecutableInternalRepresentationKHR emptyForm{
            VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR };
        std::vector<VkPipelineExecutableInternalRepresentationKHR> representations(forms, emptyForm);
        // Asked twice, as the extension has it: once for the sizes, once for the text.
        if (g.getExecutableRepresentations(g.device, &which, &forms, representations.data()) < 0) continue;
        std::vector<std::vector<char>> text(forms);
        for (uint32_t f = 0; f < forms; ++f)
        {
            text[f].resize(representations[f].dataSize);
            representations[f].pData = text[f].data();
        }
        if (g.getExecutableRepresentations(g.device, &which, &forms, representations.data()) < 0) continue;
        for (uint32_t f = 0; f < forms; ++f)
        {
            if (!representations[f].isText) continue;
            std::string name;
            token(name, executables[i].name);
            name += '.';
            token(name, representations[f].name);
            for (char& c : name) if (c == '/' || c == '(' || c == ')') c = '-';
            char path[512];
            std::snprintf(path, sizeof path, "%s/%016llx_%016llx.%s.txt", g.shaderIr,
                          (unsigned long long)vertexHash, (unsigned long long)pixelHash, name.c_str());
            if (FILE* file = std::fopen(path, "wb"))
            {
                std::fwrite(text[f].data(), 1, strnlen(text[f].data(), text[f].size()), file);
                std::fclose(file);
            }
        }
    }
}

std::string vk::pipeline::DeviceId(void* physical)
{
    VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(static_cast<VkPhysicalDevice>(physical), &properties);
    std::string text;
    for (uint8_t byte : id.deviceUUID)
    {
        char hex[3];
        std::snprintf(hex, sizeof hex, "%02x", byte);
        text += hex;
    }
    return text;
}

void* vk::pipeline::ChooseDevice(void* instance, const std::string& id, uint32_t* familyOut)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(static_cast<VkInstance>(instance), &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(static_cast<VkInstance>(instance), &count, devices.data());
    // The device the game's window would draw on (presenter.cpp): the first
    // discrete one, else the first of any kind, with a graphics queue -- or
    // the one asked for, which the game names when it draws on another.
    for (int pass = 0; pass < 2; pass++)
        for (VkPhysicalDevice candidate : devices)
        {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(candidate, &p);
            if (!id.empty() ? DeviceId(candidate) != id : pass == 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                continue;
            const uint32_t graphics = GraphicsFamily(candidate);
            if (graphics == UINT32_MAX) continue;
            if (familyOut) *familyOut = graphics;
            return candidate;
        }
    return nullptr;
}

bool vk::pipeline::Initialise(void* device, void* physical, void* queue, uint32_t queueFamily,
                              void* instance)
{
    if (g.device) return true;

    if (device && physical)
    {
        g.instance = static_cast<VkInstance>(instance);
        g.device = static_cast<VkDevice>(device);
        g.physical = static_cast<VkPhysicalDevice>(physical);
        g.queue = static_cast<VkQueue>(queue);
        g.family = queueFamily;
        g.ownsDevice = false;
    }
    else if (!BringUpHeadless())
    {
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(g.physical, &props);
    g.deviceProperties = props;
    g.name = props.deviceName;
    // A bug report is read against the driver's known faults, and a
    // performance report says which driver drew it.
    {
        VkPhysicalDeviceDriverProperties driver{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        if (props.apiVersion >= VK_API_VERSION_1_2)
        {
            VkPhysicalDeviceProperties2 more{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            more.pNext = &driver;
            vkGetPhysicalDeviceProperties2(g.physical, &more);
        }
        char version[32];
        std::snprintf(version, sizeof version, "%08X", props.driverVersion);
        // The device is made again with the window: the same strings, which
        // another thread may be reading.
        if (g.driverName != driver.driverName) g.driverName = driver.driverName;
        const std::string info = driver.driverInfo[0] ? driver.driverInfo : version;
        if (g.driverInfo != info) g.driverInfo = info;
        if (report::On())
            LOGI("report: graphics %s (%04X:%04X), driver %s %s (%08X), Vulkan %u.%u.%u", props.deviceName,
                 props.vendorID, props.deviceID, driver.driverName, driver.driverInfo, props.driverVersion,
                 VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                 VK_API_VERSION_PATCH(props.apiVersion));
    }

    if (!CreateSetLayouts()) { Shutdown(); return false; }
    CreateCache();
    LOGI("pipeline: layout ready on %s", g.name.c_str());
    return true;
}

void vk::pipeline::Shutdown()
{
    if (!g.device) return;
    ::SaveCache(true);
    {
        std::unique_lock lock(c.lock);
        if (g.cache) vkDestroyPipelineCache(g.device, g.cache, nullptr);
        for (ClosedCache& closed : c.closed) vkDestroyPipelineCache(g.device, closed.cache, nullptr);
        c.closed.clear();
        g.cache = VK_NULL_HANDLE;
        g.cacheSavedBytes = 0;
    }
    if (g.layout) vkDestroyPipelineLayout(g.device, g.layout, nullptr);
    for (VkDescriptorSetLayout& set : g.sets)
        if (set) { vkDestroyDescriptorSetLayout(g.device, set, nullptr); set = VK_NULL_HANDLE; }
    g.layout = VK_NULL_HANDLE;
    if (g.ownsDevice)
    {
#if defined(MW2_VULKAN_DYNAMIC)
        vk::loader::ForgetDevice();
#endif
        vkDestroyDevice(g.device, nullptr);
        if (g.instance) vkDestroyInstance(g.instance, nullptr);
        g.instance = VK_NULL_HANDLE;
    }
    g.device = VK_NULL_HANDLE;
    g.physical = VK_NULL_HANDLE;
    g.queue = VK_NULL_HANDLE;
    g.cacheLoaded = false;
}

bool vk::pipeline::Ready() { return g.device != VK_NULL_HANDLE; }
const char* vk::pipeline::DeviceName() { return g.name.c_str(); }
std::string vk::pipeline::DriverName() { return g.driverName; }
std::string vk::pipeline::DriverVersion() { return g.driverInfo; }
void*    vk::pipeline::Instance() { return g.instance; }
void*    vk::pipeline::Device() { return g.device; }
void*    vk::pipeline::PhysicalDevice() { return g.physical; }
void*    vk::pipeline::Queue() { return g.queue; }
uint32_t vk::pipeline::QueueFamily() { return g.family; }
bool     vk::pipeline::PreciseOcclusionQueries() { return g.preciseQueries; }
float    vk::pipeline::MaxAnisotropy() { return g.maxAnisotropy; }
bool     vk::pipeline::NonSeamlessCubes() { return g.nonSeamlessCubes; }
uint32_t vk::pipeline::MultiDraw() { return g.multiDraw; }
uint32_t vk::pipeline::Samplers() { return g.samplers; }
// MW2_NO_PIPELINE_LIBRARIES=1 builds whole pipelines at the first draw instead.
bool     vk::pipeline::PipelineLibraries()
{
    static const bool refused = env::Flag("MW2_NO_PIPELINE_LIBRARIES");
    return g.pipelineLibraries && g.fastLinking && !refused;
}
bool     vk::pipeline::HasDynamicRendering() { return g.hasDynamicRendering; }
bool     vk::pipeline::HasExtendedDynamicState() { return g.hasExtendedDynamicState; }
bool     vk::pipeline::LegacyMode() { return g.legacyMode; }
bool     vk::pipeline::TextureCompressionBC() { return g.textureCompressionBC; }
bool     vk::pipeline::SubgroupPixelRecords() { return g.subgroupPixelRecords; }
bool     vk::pipeline::UniformPixelConstants() { return g.uniformPixelConstants; }
bool     vk::pipeline::HostOrderVertices()
{
    static const bool refused = env::Text("MW2_HOST_ORDER") && !env::Flag("MW2_HOST_ORDER");
    return !refused && !g.arenaShadow;
}
void     vk::pipeline::ArenaHasShadow(bool has) { g.arenaShadow = has; }
bool     vk::pipeline::VertexTexels() { return g.vertexTexels; }
bool     vk::pipeline::PixelTexels() { return g.pixelTexels; }
bool     vk::pipeline::MirrorClampToEdge() { return g.mirrorClamp; }
void     vk::pipeline::NoteFallback(const char* what)
{
    // Once each: the pipeline cache is made again when the window is.
    if (std::find(g.fallbacks.begin(), g.fallbacks.end(), what) == g.fallbacks.end())
        g.fallbacks.emplace_back(what);
}
std::string vk::pipeline::DeviceSummary()
{
    std::string list;
    for (const std::string& fallback : g.fallbacks)
    {
        if (!list.empty()) list += "; ";
        list += fallback;
    }
    return g.deviceClass + "; fallbacks: " + (list.empty() ? "none" : list);
}
void     vk::pipeline::LogDeviceSummary() { LOGI("device: %s", DeviceSummary().c_str()); }
const char* vk::pipeline::Refusal() { return g.refusal; }
std::mutex& vk::pipeline::QueueMutex() { static std::mutex m; return m; }
void*    vk::pipeline::SetLayout(uint32_t set) { return set < 3 ? g.sets[set] : nullptr; }
void*    vk::pipeline::Layout() { return g.layout; }
void     vk::pipeline::SaveCache() { ::SaveCache(); }
bool     vk::pipeline::OneCreation() { return g.oneCreation; }

int vk::pipeline::CreateGraphicsPipeline(const void* createInfo, void* pipeline, int* fromCache)
{
    VkGraphicsPipelineCreateInfo info = *static_cast<const VkGraphicsPipelineCreateInfo*>(createInfo);
    VkPipeline* made = static_cast<VkPipeline*>(pipeline);
    if (fromCache) *fromCache = -1;
    CreationLock lock(c.lock);
    // A library of state alone and a fast link compile nothing: every cache
    // would answer them, and none would have been of use.
    const bool compiles = info.stageCount || (info.flags & VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT);
    if (g.cache && g.cacheControl && compiles)
    {
        const VkPipelineCreateFlags flags = info.flags;
        info.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        VkResult result = vkCreateGraphicsPipelines(g.device, g.cache, 1, &info, nullptr, made);
        const uint32_t count = uint32_t(c.closed.size());
        const uint32_t first = c.lastFound.load(std::memory_order_relaxed);
        for (uint32_t i = 0; result == VK_PIPELINE_COMPILE_REQUIRED && i < count; i++)
        {
            const uint32_t at = (first + i) % count;
            result = vkCreateGraphicsPipelines(g.device, c.closed[at].cache, 1, &info, nullptr, made);
            if (result != VK_SUCCESS) continue;
            c.closed[at].foundNow.fetch_add(1, std::memory_order_relaxed);
            c.foundClosed.fetch_add(1, std::memory_order_relaxed);
            c.lastFound.store(at, std::memory_order_relaxed);
        }
        if (result != VK_PIPELINE_COMPILE_REQUIRED)
        {
            if (result == VK_SUCCESS)
            {
                c.found.fetch_add(1, std::memory_order_relaxed);
                if (fromCache) *fromCache = 1;
            }
            return result;
        }
        c.compiled.fetch_add(1, std::memory_order_relaxed);
        if (fromCache) *fromCache = 0;
        info.flags = flags;
    }
    return vkCreateGraphicsPipelines(g.device, g.cache, 1, &info, nullptr, made);
}

bool vk::pipeline::CreateModule(const uint32_t* spirv, size_t words, const char** error)
{
    if (!g.device) { if (error) *error = "no device"; return false; }
    VkResult r;
    VkShaderModule module = MakeModule(spirv, words, r);
    if (r != VK_SUCCESS)
    {
        g.lastError = Explain(r);
        if (error) *error = g.lastError.c_str();
        return false;
    }
    vkDestroyShaderModule(g.device, module, nullptr);
    return true;
}

#endif  // MW2_HAVE_VULKAN
