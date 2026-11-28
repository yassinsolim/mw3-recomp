// Whether a driver can run the renderer, asked before the game is.
//
// Players on chips nobody here has were left at a black screen by a driver
// that loaded and showed nothing. This is the short answer to "does this
// driver draw": in a process of its own (DriverSelfTest.kt -- a driver that
// crashes takes that process and nothing else), the driver is opened the
// way a run opens it, the device is made by the renderer's own
// vk::pipeline::CreateDevice, which refuses what the renderer cannot use,
// and one small frame is drawn off screen and read back:
//
//   - shaders built as the renderer's are: no vertex attributes, the draw's
//     record found through a draw table at its base vertex and its offset
//     passed on to the pixel stage, positions and the record read from a
//     storage buffer, the texture taken out of one 65,536-image set that is
//     updated after it is bound and indexed non-uniformly (bindings.h);
//   - a BC1 texture sampled by the driver, where the renderer would let it;
//   - the alpha test as a discard, the depth test, blending;
//   - a compute pipeline (the renderer has one, display_table.cpp).
//
// Every pixel checked has one right answer. What is written to the report
// file, a line a step and flushed at once, is what the app reads: a process
// that died says how far it got.
#ifdef MW2_ANDROID

#include "../../android/android.h"
#include "bindings.h"
#include "pipeline.h"
#include "util.h"
#include "../../log.h"

#include <unistd.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(MW2_HAVE_VULKAN) && defined(MW2_VULKAN_DYNAMIC)

namespace
{
#include "shaders/selftest_shaders.inc"

    constexpr uint32_t kSize = 64;
    // Where the two textures sit in the set: far apart, as the renderer's do.
    constexpr uint32_t kCompressedImage = 40000, kPlainImage = 3;
    constexpr uint32_t kNearest = 4000;
    constexpr uint32_t kComputeValues = 64;

    struct Report
    {
        std::FILE* file = nullptr;
        void Line(const char* format, ...) __attribute__((format(printf, 2, 3)))
        {
            char text[768];
            va_list arguments;
            va_start(arguments, format);
            std::vsnprintf(text, sizeof text, format, arguments);
            va_end(arguments);
            LOGI("driver: self-test: %s", text);
            if (!file) return;
            std::fprintf(file, "%s\n", text);
            std::fflush(file);
            fsync(fileno(file));
        }
    };

    struct Test
    {
        Report report;
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t family = 0;
        VkPhysicalDeviceMemoryProperties memory{};
        std::string failure;

        bool Fail(const char* format, ...) __attribute__((format(printf, 2, 3)))
        {
            char text[512];
            va_list arguments;
            va_start(arguments, format);
            std::vsnprintf(text, sizeof text, format, arguments);
            va_end(arguments);
            if (failure.empty()) failure = text;
            return false;
        }
        bool Ok(VkResult result, const char* what)
        {
            return result == VK_SUCCESS || Fail("%s failed (%d)", what, int(result));
        }
    };

    bool HasInstanceExtension(const char* name)
    {
        uint32_t count = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || !count) return false;
        std::vector<VkExtensionProperties> offered(count);
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, offered.data()) != VK_SUCCESS) return false;
        for (const auto& extension : offered)
            if (std::strcmp(extension.extensionName, name) == 0) return true;
        return false;
    }

    // The instance the presenter makes (presenter.cpp, CreateInstance): the
    // surface extensions named, the highest version the driver admits to.
    bool MakeInstance(Test& t)
    {
        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "mw2recomp";
        app.apiVersion = VK_API_VERSION_1_0;
        uint32_t available = 0;
        if (vkEnumerateInstanceVersion && vkEnumerateInstanceVersion(&available) == VK_SUCCESS)
            app.apiVersion = available >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3
                           : available >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
        std::vector<const char*> extensions;
        for (const char* name : { VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_android_surface" })
        {
            if (!HasInstanceExtension(name))
                return t.Fail("the driver has no %s: it cannot present to the screen", name);
            extensions.push_back(name);
        }
        if (app.apiVersion < VK_API_VERSION_1_1)
            for (const char* name : { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                                      VK_KHR_MAINTENANCE1_EXTENSION_NAME })
                if (HasInstanceExtension(name)) extensions.push_back(name);
        VkInstanceCreateInfo info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = uint32_t(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        if (!t.Ok(vkCreateInstance(&info, nullptr, &t.instance), "vkCreateInstance")) return false;
        vk::loader::LoadInstance(t.instance);
        return true;
    }

    bool PickDevice(Test& t)
    {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(t.instance, &count, nullptr);
        if (!count) return t.Fail("the driver offers no graphics device");
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(t.instance, &count, devices.data());
        for (VkPhysicalDevice candidate : devices)
        {
            const uint32_t family = vk::pipeline::GraphicsFamily(candidate);
            if (family == UINT32_MAX) continue;
            t.physical = candidate;
            t.family = family;
            break;
        }
        if (!t.physical) return t.Fail("the driver offers no device that can draw");

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(t.physical, &properties);
        VkPhysicalDeviceDriverProperties driver{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        if (vkGetPhysicalDeviceProperties2)
        {
            VkPhysicalDeviceProperties2 more{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            more.pNext = &driver;
            vkGetPhysicalDeviceProperties2(t.physical, &more);
        }
        t.report.Line("device=%s", properties.deviceName);
        t.report.Line("vendor=0x%04X", properties.vendorID);
        t.report.Line("deviceId=0x%08X", properties.deviceID);
        t.report.Line("driverVersion=0x%08X", properties.driverVersion);
        t.report.Line("driverName=%s", driver.driverName);
        t.report.Line("driverInfo=%s", driver.driverInfo);
        t.report.Line("api=%u.%u.%u", VK_VERSION_MAJOR(properties.apiVersion),
                      VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion));
        vkGetPhysicalDeviceMemoryProperties(t.physical, &t.memory);
        return true;
    }

    VkDeviceMemory Allocate(Test& t, const VkMemoryRequirements& needs, VkMemoryPropertyFlags want)
    {
        VkMemoryAllocateInfo info{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        info.allocationSize = needs.size;
        info.memoryTypeIndex = vk::util::FindMemory(t.memory, needs.memoryTypeBits, want);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (info.memoryTypeIndex == UINT32_MAX) { t.Fail("no memory type for the test's images"); return VK_NULL_HANDLE; }
        t.Ok(vkAllocateMemory(t.device, &info, nullptr, &memory), "vkAllocateMemory");
        return memory;
    }

    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };

    Buffer MakeBuffer(Test& t, VkDeviceSize size, VkBufferUsageFlags usage)
    {
        Buffer made;
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage;
        if (!t.Ok(vkCreateBuffer(t.device, &info, nullptr, &made.buffer), "vkCreateBuffer")) return made;
        VkMemoryRequirements needs{};
        vkGetBufferMemoryRequirements(t.device, made.buffer, &needs);
        // Every device has a type that is both (the specification's own promise).
        VkDeviceMemory memory = Allocate(t, needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!memory) return made;
        if (!t.Ok(vkBindBufferMemory(t.device, made.buffer, memory, 0), "vkBindBufferMemory")) return made;
        t.Ok(vkMapMemory(t.device, memory, 0, VK_WHOLE_SIZE, 0, &made.mapped), "vkMapMemory");
        return made;
    }

    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    Image MakeImage(Test& t, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                    VkImageAspectFlags aspect)
    {
        Image made;
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!t.Ok(vkCreateImage(t.device, &info, nullptr, &made.image), "vkCreateImage")) return made;
        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(t.device, made.image, &needs);
        VkDeviceMemory memory = Allocate(t, needs, 0);
        if (!memory) return made;
        if (!t.Ok(vkBindImageMemory(t.device, made.image, memory, 0), "vkBindImageMemory")) return made;
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = made.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = { aspect, 0, 1, 0, 1 };
        t.Ok(vkCreateImageView(t.device, &view, nullptr, &made.view), "vkCreateImageView");
        return made;
    }

    template <size_t N>
    VkShaderModule Module(Test& t, const uint32_t (&words)[N])
    {
        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        info.codeSize = sizeof words;
        info.pCode = words;
        VkShaderModule module = VK_NULL_HANDLE;
        t.Ok(vkCreateShaderModule(t.device, &info, nullptr, &module), "vkCreateShaderModule");
        return module;
    }

    uint32_t Bits(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        return bits;
    }

    // One draw of the frame: a rectangle in clip space at a depth, with its
    // record (selftest.frag: image, mode, sampler, tint).
    struct Quad
    {
        float x0, y0, x1, y1, z;
        uint32_t image, mode, sampler;
        float tint[4];
        bool blended;
    };

    struct Expected
    {
        uint32_t x, y;
        uint8_t r, g, b;
        const char* what;
    };

    bool Draw(Test& t)
    {
        const bool compressed = vk::pipeline::TextureCompressionBC();
        t.report.Line("bc1=%s", compressed ? "sampled by the driver" : "decoded by the runtime (not tested here)");

        // ---- the target and what is sampled --------------------------------
        Image colour = MakeImage(t, VK_FORMAT_R8G8B8A8_UNORM, kSize, kSize,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
        // D16 is the one depth format every device must draw to.
        Image depth = MakeImage(t, VK_FORMAT_D16_UNORM, kSize, kSize,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        // One block of solid green, as DXT1 or as the texels the runtime's decoder would hand over.
        Image green = compressed
            ? MakeImage(t, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, sampled, VK_IMAGE_ASPECT_COLOR_BIT)
            : MakeImage(t, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, sampled, VK_IMAGE_ASPECT_COLOR_BIT);
        // Two texels: a transparent one the alpha test drops, an opaque blue one.
        Image cutout = MakeImage(t, VK_FORMAT_R8G8B8A8_UNORM, 2, 1, sampled, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!t.failure.empty()) return false;

        Buffer upload = MakeBuffer(t, 256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer readback = MakeBuffer(t, kSize * kSize * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Buffer arena = MakeBuffer(t, 4096 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Buffer values = MakeBuffer(t, kComputeValues * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!t.failure.empty()) return false;

        auto* bytes = static_cast<uint8_t*>(upload.mapped);
        constexpr uint32_t kGreenAt = 0, kCutoutAt = 64;
        if (compressed)
        {
            // colour0 = colour1 = 0x07E0, every texel the first.
            const uint8_t block[8] = { 0xE0, 0x07, 0xE0, 0x07, 0, 0, 0, 0 };
            std::memcpy(bytes + kGreenAt, block, sizeof block);
        }
        else
        {
            for (uint32_t i = 0; i < 16; i++)
            {
                const uint8_t texel[4] = { 0, 255, 0, 255 };
                std::memcpy(bytes + kGreenAt + i * 4, texel, 4);
            }
        }
        const uint8_t pair[8] = { 255, 255, 255, 0, 0, 0, 255, 255 };
        std::memcpy(bytes + kCutoutAt, pair, sizeof pair);

        const Quad quads[] = {
            // Top left: red, then half-transparent white blended over its corner.
            { -1, -1, 0, 0, 0.5f, 0, 0, 0, { 1, 0, 0, 1 }, false },
            // Top right: the compressed texture.
            { 0, -1, 1, 0, 0.5f, kCompressedImage, 1, kNearest, { 1, 1, 1, 1 }, false },
            // Bottom left: the alpha test, its left half dropped.
            { -1, 0, 0, 1, 0.5f, kPlainImage, 2, kNearest, { 1, 1, 1, 1 }, false },
            // Bottom right: yellow in front, magenta behind it drawn after: the depth test keeps yellow.
            { 0, 0, 1, 1, 0.25f, 0, 0, 0, { 1, 1, 0, 1 }, false },
            { 0, 0, 1, 1, 0.75f, 0, 0, 0, { 1, 0, 1, 1 }, false },
            { -1, -1, -0.5f, -0.5f, 0.125f, 0, 0, 0, { 1, 1, 1, 0.5f }, true },
        };
        constexpr uint32_t kQuads = sizeof quads / sizeof quads[0];
        // The records first, eight words each, then six vertices of eight
        // words a quad, then the draw table (selftest.vert): an entry a quad,
        // the record's dword offset and where its vertices are, found at the
        // quad's draw's base vertex.
        auto* words = static_cast<uint32_t*>(arena.mapped);
        std::memset(words, 0, 4096 * 4);
        constexpr uint32_t kVertexWords = 64, kTableWords = 512;
        for (uint32_t q = 0; q < kQuads; q++)
        {
            const Quad& quad = quads[q];
            uint32_t* record = words + q * 8;
            const uint32_t at = kVertexWords + q * 48;
            words[kTableWords + q * 2] = q * 8;
            words[kTableWords + q * 2 + 1] = at;
            record[1] = quad.image;
            record[2] = quad.mode;
            record[3] = quad.sampler;
            for (int i = 0; i < 4; i++) record[4 + i] = Bits(quad.tint[i]);
            const float corners[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
            for (uint32_t v = 0; v < 6; v++)
            {
                uint32_t* vertex = words + at + v * 8;
                vertex[0] = Bits(quad.x0 + (quad.x1 - quad.x0) * corners[v][0]);
                vertex[1] = Bits(quad.y0 + (quad.y1 - quad.y0) * corners[v][1]);
                vertex[2] = Bits(quad.z);
                vertex[4] = Bits(corners[v][0]);
                vertex[5] = Bits(corners[v][1]);
            }
        }
        auto* numbers = static_cast<uint32_t*>(values.mapped);
        for (uint32_t i = 0; i < kComputeValues; i++) numbers[i] = i * 7;

        // ---- the sets --------------------------------------------------------
        VkDescriptorSetLayoutBinding arenaBinding{};
        arenaBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        arenaBinding.descriptorCount = 1;
        arenaBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                  VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo arenaInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        arenaInfo.bindingCount = 1;
        arenaInfo.pBindings = &arenaBinding;
        VkDescriptorSetLayout arenaLayout = VK_NULL_HANDLE;
        if (!t.Ok(vkCreateDescriptorSetLayout(t.device, &arenaInfo, nullptr, &arenaLayout),
                  "vkCreateDescriptorSetLayout")) return false;

        // The renderer's texture set, at its size and with its flags.
        VkDescriptorSetLayoutBinding textureBindings[2]{};
        textureBindings[0].binding = vk::bindings::kImages;
        textureBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        textureBindings[0].descriptorCount = vk::bindings::kImageCount;
        textureBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        textureBindings[1].binding = vk::bindings::kSamplers;
        textureBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        textureBindings[1].descriptorCount = vk::bindings::kSamplerCount;
        textureBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // Both as the renderer's own (pipeline.cpp).
        const VkDescriptorBindingFlags flags[2] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        flagsInfo.bindingCount = 2;
        flagsInfo.pBindingFlags = flags;
        VkDescriptorSetLayoutCreateInfo textureInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        textureInfo.pNext = &flagsInfo;
        textureInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        textureInfo.bindingCount = 2;
        textureInfo.pBindings = textureBindings;
        VkDescriptorSetLayout textureLayout = VK_NULL_HANDLE;
        if (!t.Ok(vkCreateDescriptorSetLayout(t.device, &textureInfo, nullptr, &textureLayout),
                  "vkCreateDescriptorSetLayout (65,536 textures, updated after they are bound)")) return false;

        const VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, vk::bindings::kImageCount },
            { VK_DESCRIPTOR_TYPE_SAMPLER, vk::bindings::kSamplerCount },
        };
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets = 3;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (!t.Ok(vkCreateDescriptorPool(t.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool")) return false;
        const VkDescriptorSetLayout layouts[3] = { arenaLayout, textureLayout, arenaLayout };
        VkDescriptorSet sets[3]{};
        VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocate.descriptorPool = pool;
        allocate.descriptorSetCount = 3;
        allocate.pSetLayouts = layouts;
        if (!t.Ok(vkAllocateDescriptorSets(t.device, &allocate, sets), "vkAllocateDescriptorSets")) return false;

        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkSampler sampler = VK_NULL_HANDLE;
        if (!t.Ok(vkCreateSampler(t.device, &samplerInfo, nullptr, &sampler), "vkCreateSampler")) return false;

        const VkDescriptorBufferInfo arenaView{ arena.buffer, 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo valuesView{ values.buffer, 0, VK_WHOLE_SIZE };
        const VkDescriptorImageInfo greenView{ VK_NULL_HANDLE, green.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo cutoutView{ VK_NULL_HANDLE, cutout.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo samplerView{ sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet writes[5]{};
        for (auto& write : writes)
        {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.descriptorCount = 1;
        }
        writes[0].dstSet = sets[0];
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &arenaView;
        writes[1].dstSet = sets[2];
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &valuesView;
        writes[2].dstSet = sets[1];
        writes[2].dstBinding = vk::bindings::kSamplers;
        writes[2].dstArrayElement = kNearest;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[2].pImageInfo = &samplerView;
        // The two images are written after the set is bound, below.
        writes[3].dstSet = sets[1];
        writes[3].dstBinding = vk::bindings::kImages;
        writes[3].dstArrayElement = kCompressedImage;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[3].pImageInfo = &greenView;
        writes[4] = writes[3];
        writes[4].dstArrayElement = kPlainImage;
        writes[4].pImageInfo = &cutoutView;
        vkUpdateDescriptorSets(t.device, 3, writes, 0, nullptr);

        // ---- the pass and its pipelines --------------------------------------
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        attachments[1] = attachments[0];
        attachments[1].format = VK_FORMAT_D16_UNORM;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference colourReference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        const VkAttachmentReference depthReference{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colourReference;
        subpass.pDepthStencilAttachment = &depthReference;
        VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        passInfo.attachmentCount = 2;
        passInfo.pAttachments = attachments;
        passInfo.subpassCount = 1;
        passInfo.pSubpasses = &subpass;
        VkRenderPass pass = VK_NULL_HANDLE;
        if (!t.Ok(vkCreateRenderPass(t.device, &passInfo, nullptr, &pass), "vkCreateRenderPass")) return false;
        const VkImageView views[2] = { colour.view, depth.view };
        VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = pass;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = views;
        framebufferInfo.width = framebufferInfo.height = kSize;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (!t.Ok(vkCreateFramebuffer(t.device, &framebufferInfo, nullptr, &framebuffer), "vkCreateFramebuffer"))
            return false;

        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 2;
        layoutInfo.pSetLayouts = layouts;
        VkPipelineLayout layout = VK_NULL_HANDLE, computeLayout = VK_NULL_HANDLE;
        if (!t.Ok(vkCreatePipelineLayout(t.device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout"))
            return false;
        layoutInfo.setLayoutCount = 1;
        if (!t.Ok(vkCreatePipelineLayout(t.device, &layoutInfo, nullptr, &computeLayout), "vkCreatePipelineLayout"))
            return false;

        t.report.Line("stage=shaders");
        const VkShaderModule vertex = Module(t, kSelfTestVertex);
        const VkShaderModule fragment = Module(t, kSelfTestFragment);
        const VkShaderModule compute = Module(t, kSelfTestCompute);
        if (!t.failure.empty()) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[0].pName = stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        const VkViewport viewport{ 0, 0, float(kSize), float(kSize), 0, 1 };
        const VkRect2D scissor{ { 0, 0 }, { kSize, kSize } };
        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthState{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depthState.depthTestEnable = VK_TRUE;
        depthState.depthWriteEnable = VK_TRUE;
        depthState.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo blendState{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blendState.attachmentCount = 1;
        blendState.pAttachments = &blend;
        VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthState;
        pipelineInfo.pColorBlendState = &blendState;
        pipelineInfo.layout = layout;
        pipelineInfo.renderPass = pass;
        t.report.Line("stage=pipelines");
        VkPipeline opaque = VK_NULL_HANDLE, blended = VK_NULL_HANDLE, computePipeline = VK_NULL_HANDLE;
        if (!t.Ok(vkCreateGraphicsPipelines(t.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &opaque),
                  "vkCreateGraphicsPipelines")) return false;
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
        depthState.depthWriteEnable = VK_FALSE;
        if (!t.Ok(vkCreateGraphicsPipelines(t.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &blended),
                  "vkCreateGraphicsPipelines (blended)")) return false;
        VkComputePipelineCreateInfo computeInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        computeInfo.stage = stages[0];
        computeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        computeInfo.stage.module = compute;
        computeInfo.layout = computeLayout;
        if (!t.Ok(vkCreateComputePipelines(t.device, VK_NULL_HANDLE, 1, &computeInfo, nullptr, &computePipeline),
                  "vkCreateComputePipelines")) return false;

        // ---- the frame ---------------------------------------------------------
        t.report.Line("stage=draw");
        VkCommandPoolCreateInfo commandPoolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        commandPoolInfo.queueFamilyIndex = t.family;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        if (!t.Ok(vkCreateCommandPool(t.device, &commandPoolInfo, nullptr, &commandPool), "vkCreateCommandPool"))
            return false;
        VkCommandBufferAllocateInfo commandInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        commandInfo.commandPool = commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        if (!t.Ok(vkAllocateCommandBuffers(t.device, &commandInfo, &commands), "vkAllocateCommandBuffers"))
            return false;
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commands, &begin);

        struct Upload { VkImage image; uint32_t at, width, height; };
        for (const Upload& one : { Upload{ green.image, kGreenAt, 4, 4 }, Upload{ cutout.image, kCutoutAt, 2, 1 } })
        {
            vk::util::Barrier(commands, one.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.bufferOffset = one.at;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { one.width, one.height, 1 };
            vkCmdCopyBufferToImage(commands, upload.buffer, one.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            vk::util::Barrier(commands, one.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout, 0, 1, &sets[2], 0, nullptr);
        vkCmdDispatch(commands, kComputeValues / 16, 1, 1);

        VkClearValue clears[2]{};
        clears[0].color.float32[0] = 0.1f;
        clears[0].color.float32[1] = 0.2f;
        clears[0].color.float32[2] = 0.3f;
        clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil.depth = 1.0f;
        VkRenderPassBeginInfo passBegin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        passBegin.renderPass = pass;
        passBegin.framebuffer = framebuffer;
        passBegin.renderArea = scissor;
        passBegin.clearValueCount = 2;
        passBegin.pClearValues = clears;
        vkCmdBeginRenderPass(commands, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 0, nullptr);
        // As the renderer does: a texture's place in the set is written with the set bound.
        vkUpdateDescriptorSets(t.device, 2, writes + 3, 0, nullptr);
        for (uint32_t q = 0; q < kQuads; q++)
        {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, quads[q].blended ? blended : opaque);
            // The base vertex is where the quad's draw table entry is (selftest.vert).
            vkCmdDraw(commands, 6, 1, kTableWords + q * 2, 0);
        }
        vkCmdEndRenderPass(commands);

        VkBufferImageCopy out{};
        out.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        out.imageExtent = { kSize, kSize, 1 };
        vkCmdCopyImageToBuffer(commands, colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &out);
        if (!t.Ok(vkEndCommandBuffer(commands), "vkEndCommandBuffer")) return false;

        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence fence = VK_NULL_HANDLE;
        if (!t.Ok(vkCreateFence(t.device, &fenceInfo, nullptr, &fence), "vkCreateFence")) return false;
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        if (!t.Ok(vkQueueSubmit(t.queue, 1, &submit, fence), "vkQueueSubmit")) return false;
        const VkResult waited = vkWaitForFences(t.device, 1, &fence, VK_TRUE, 10'000'000'000ull);
        if (waited == VK_TIMEOUT) return t.Fail("the graphics chip did not finish the test frame in 10 s");
        if (!t.Ok(waited, "vkWaitForFences")) return false;

        // ---- what came back ------------------------------------------------------
        t.report.Line("stage=compare");
        for (uint32_t i = 0; i < kComputeValues; i++)
            if (numbers[i] != i * 7 * 3 + i + 1)
                return t.Fail("the compute shader wrote %u at %u where %u belongs", numbers[i], i, i * 7 * 3 + i + 1);

        const Expected expected[] = {
            { 8, 8, 255, 128, 128, "white blended over red" },
            { 24, 24, 255, 0, 0, "the red rectangle" },
            { 48, 16, 0, 255, 0, compressed ? "the BC1 texture" : "the green texture" },
            { 8, 48, 26, 51, 77, "the alpha test's dropped half (the clear colour)" },
            { 24, 48, 0, 0, 255, "the alpha test's kept half" },
            { 48, 48, 255, 255, 0, "the depth test (yellow in front of magenta)" },
        };
        const auto* pixels = static_cast<const uint8_t*>(readback.mapped);
        std::string wrong;
        for (const Expected& e : expected)
        {
            const uint8_t* p = pixels + (e.y * kSize + e.x) * 4;
            const bool right = std::abs(int(p[0]) - e.r) <= 8 && std::abs(int(p[1]) - e.g) <= 8 &&
                               std::abs(int(p[2]) - e.b) <= 8;
            t.report.Line("pixel=%u,%u %02X%02X%02X want %02X%02X%02X %s: %s", e.x, e.y, p[0], p[1], p[2],
                          e.r, e.g, e.b, right ? "ok" : "WRONG", e.what);
            if (!right && wrong.empty())
            {
                char text[200];
                std::snprintf(text, sizeof text, "%s came out #%02X%02X%02X, not #%02X%02X%02X", e.what,
                              p[0], p[1], p[2], e.r, e.g, e.b);
                wrong = text;
            }
        }
        if (!wrong.empty()) return t.Fail("the test frame is wrong: %s", wrong.c_str());
        return true;
    }
}

std::string android::driver::SelfTest(const char* reportPath)
{
    Test t;
    t.report.file = reportPath && *reportPath ? std::fopen(reportPath, "w") : nullptr;
    t.report.Line("stage=open");
    std::string error;
    bool ok = vk::loader::Opened() || vk::loader::Open(error);
    if (!ok) t.Fail("no Vulkan driver could be opened: %s", error.c_str());
    t.report.Line("driver=%s", Description().c_str());
    // The chosen driver would not load and the system's was opened in its
    // place: the test is of the chosen one, and it has failed.
    if (ok && CustomDriverFailed()) ok = t.Fail("the driver would not load");
    if (ok)
    {
        t.report.Line("stage=instance");
        ok = MakeInstance(t);
    }
    if (ok)
    {
        t.report.Line("stage=device");
        ok = PickDevice(t);
    }
    if (ok)
    {
        const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        void* device = nullptr;
        void* queue = nullptr;
        if (!vk::pipeline::DeviceHasExtension(t.physical, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            ok = t.Fail("the driver has no swapchain: it cannot present to the screen");
        else if (!vk::pipeline::CreateDevice(t.physical, t.family, extensions, 1, &device, &queue))
        {
            const char* lacks = vk::pipeline::Refusal();
            ok = *lacks ? t.Fail("this graphics chip or its driver lacks %s, which the renderer needs", lacks)
                        : t.Fail("the driver would not make a device (vkCreateDevice failed)");
        }
        else
        {
            t.device = static_cast<VkDevice>(device);
            t.queue = static_cast<VkQueue>(queue);
            t.report.Line("summary=%s", vk::pipeline::DeviceSummary().c_str());
        }
    }
    if (ok) ok = Draw(t);
    // The last line, and the only one the verdict is read from. Nothing is
    // destroyed: the process ends with the test (DriverTestService).
    if (ok && t.failure.empty()) t.report.Line("result=pass");
    else t.report.Line("result=fail:%s", t.failure.empty() ? "unknown" : t.failure.c_str());
    if (t.report.file) std::fclose(t.report.file);
    return t.failure;
}

#else

std::string android::driver::SelfTest(const char*) { return "this build has no Vulkan"; }

#endif
#endif  // MW2_ANDROID
