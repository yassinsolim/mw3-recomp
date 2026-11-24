#include "presenter.h"
#include "../../diagnostics.h"
#include "../../env.h"
#include "../../frame_rate.h"
#include "../../log.h"
#include "../../crash.h"
#include "../../console.h"
#include "../../stutters.h"
#include "../../friends.h"
#include "../../guide.h"
#include "../../signin.h"
#include "../../render_size.h"
#include "backdrop.h"
#include "sharpen.h"

#if !defined(MW2_HAVE_VULKAN) || (!defined(MW2_USE_SDL) && !defined(MW2_ANDROID))
// Without a loader or a window system there is nothing to present to; the rest
// of the runtime carries on headless.
bool     vk::Start()           { return false; }
void     vk::ShowWindow()      {}
void     vk::StopPresenting()  {}
void     vk::Stop()            {}
void     vk::ShowImage(void*, uint32_t, uint32_t, uint64_t) {}
void     vk::WaitUntilTaken(uint64_t, bool (*)()) {}
void     vk::WaitForDisplay(uint64_t, bool (*)()) {}
uint64_t vk::GuestBlankPeriod() { return 0; }
void     vk::TakeMouse(bool, Mouse& out) { out = {}; }
bool     vk::Running()         { return false; }
uint64_t vk::PresentedFrames() { return 0; }
#else

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <chrono>
#include <algorithm>
#include <utility>
#include <vector>

#include "capture.h"
#include "gpu_time.h"
#include "pipeline.h"
#include "renderer.h"

#ifdef MW2_USE_SDL
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif
#ifdef MW2_ANDROID
#include <android/native_window.h>
#include "../../android/android.h"
// The surface extension's declarations. A build that loads Vulkan at run
// time has had this already (loader.h); one that links it has not.
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#endif
#include <vulkan/vulkan.h>
#include "loader.h"
#include "util.h"

namespace
{
    constexpr uint32_t kWidth = 1280;
    constexpr uint32_t kHeight = 720;
    // How long a screen takes to fade in or out, in seconds.
    constexpr float kFade = 0.15f;
    constexpr uint32_t kFramesInFlight = 2;

    struct Frame
    {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkSemaphore rendered = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
    };

    struct Presenter
    {
#ifdef MW2_USE_SDL
        SDL_Window* window = nullptr;
#endif
#ifdef MW2_ANDROID
        // The surface the activity owns. It goes away every time the player
        // leaves the game and comes back as a different one; `generation`
        // is how the loop notices.
        ANativeWindow* nativeWindow = nullptr;
        uint64_t generation = 0;
#endif
        VkInstance instance = VK_NULL_HANDLE;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VkQueue queue = VK_NULL_HANDLE;

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{ kWidth, kHeight };
        std::vector<VkImage> images;
        // The window changed size, or the swapchain said it no longer fits:
        // remade before the next frame. A window of no size -- minimised -- has
        // nothing to remake it for, and frames are skipped until it has one.
        bool remakeSwapchain = false;
        // The surface reports a rotation this renderer does not apply, and
        // the swapchain was made with identity on purpose (see below). Vulkan
        // answers every acquire and every present with SUBOPTIMAL for as long
        // as that is true, which is correct and permanent -- it must not be
        // read as "remake me".
        bool suboptimalIsExpected = false;
        bool fullscreen = false;
        bool stretch = false;   // MW2_STRETCH
        uint64_t swapchainsMade = 0;

        VkCommandPool pool = VK_NULL_HANDLE;
        Frame frames[kFramesInFlight]{};
        uint32_t frameIndex = 0;

        // The system's screens (screen.h), laid over the frame while one is
        // open: their pictures, the image the one shown is copied to and the
        // buffer it goes by.
        struct Overlay
        {
            screen::Image signin, friends, guide;
            const screen::Image* shown = nullptr;   // the picture the image holds
            float outline = 0;                      // and its outline's width
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory imageMemory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkBuffer staging = VK_NULL_HANDLE;
            VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
            void* mapped = nullptr;
            uint32_t width = 0, height = 0;
            uint64_t uploaded = 0;       // the picture's version the image holds
            bool everUploaded = false;
            float fade = 0;              // 0 shut to 1 shown
            bool layered = false;        // the image is laid out for the backdrop's shader
            VkOffset3D frameFrom{}, frameTo{};   // where the last game frame was shown
            std::chrono::steady_clock::time_point drawn{};
        } overlay;

        std::thread worker;
        std::atomic<bool> running{ false };
        std::atomic<bool> ready{ false };
        std::atomic<bool> showAsked{ false };
        bool shown = false;              // the window thread's own
        std::atomic<uint64_t> presented{ 0 };

        // The frames the renderer has finished and the window has not shown
        // yet, oldest first. Each is shown once, in order: the window used to
        // show whichever frame was newest when the title asked for a present,
        // and a frame finishing about then was shown twice or never.
        struct Queued
        {
            VkImage image = VK_NULL_HANDLE;
            uint32_t width = 0, height = 0;
            uint64_t serial = 0;
        };
        std::mutex lock;
        std::condition_variable wake, takenWake;
        std::deque<Queued> frameQueue;
        uint64_t taken = 0;                  // the last frame copied out or let go
        // The console's pacing at half rate (frame_rate.h): the frame on
        // screen, and how many more blanks it is to be shown for before the
        // next is taken. The renderer finishes frames in bursts when the GPU
        // is what it waits for, and shown as they came a frame stayed one
        // blank or three as often as two.
        Queued held;
        uint32_t holdsLeft = 0;
        std::atomic<uint64_t> lastQueued{ 0 };

        // When each present reaches the screen (VK_KHR_present_wait), from a
        // thread of its own, which is what the guest's blank follows.
        bool presentWait = false;
        PFN_vkWaitForPresentKHR waitForPresent = nullptr;
        std::thread waiter;
        std::mutex waitLock;
        std::condition_variable waitWake;
        std::deque<uint64_t> waiting;        // present ids not yet seen on screen
        // Held shared while waiting on the swapchain, exclusive to replace it.
        std::shared_mutex swapchainLife;

        // The display's clock, as the presents reaching it measure it.
        double nominalNs = 0;                // from the display mode; 0 = not followed
        double modeHz = 0;                   // the display mode's rate
        double periodNs = 0;
        uint32_t measured = 0;               // gaps the period was measured from
        int64_t lastShownNs = -1;
        uint64_t lastShownId = 0;
        bool exactRate = false;              // the display mode gave its rate exactly
        double buffered = 1.0;               // frames queued behind the one on screen
        uint64_t shownOnTime = 0, stretched = 0;
        uint64_t behindCount[4]{};           // 0, 1, 2, 3+ frames behind at each
        std::atomic<uint64_t> guestPeriodNs{ 0 };
        std::atomic<int64_t> guestPeriodAt{ 0 };
    };

    Presenter g;

#ifdef _WIN32
    // The mouse (presenter.h). Windows only, as reading it is, and a Windows
    // window is SDL's.
    struct WindowMouse
    {
        bool used = false;      // not under MW2_MOUSE=0
        std::mutex lock;
        bool focused = false;   // the window has the keyboard
        bool freed = false;     // F2: the player has the cursor back
        bool pointing = false;  // a menu is up and the cursor is its pointer (TakeMouse)
        bool held = false;      // relative mode is on: the window's thread sets it
        float dx = 0, dy = 0;   // counts since TakeMouse
        float wheel = 0;        // notches since TakeMouse, away from the hand positive
        float x = 0, y = 0;     // the pointer, in the window's pixels
        int32_t picture[4]{};   // the frame's left, top, right and bottom in them
    } g_mouse;

    // SDL calls this as an event is queued, on the thread that queued it. In
    // relative mode the mouse is read on a thread of SDL's own, so its counts
    // are added up here as they come, not when the window's loop next goes
    // round -- once a frame, and behind a present that may wait for the
    // display. Holding and letting go on focus are SDL's doing: a window's
    // relative mode is in force only while it has the keyboard, so Alt+Tab
    // gives the cursor back without anything here.
    bool SDLCALL WatchMouse(void*, SDL_Event* event)
    {
        switch (event->type)
        {
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        {
            std::lock_guard lock(g_mouse.lock);
            g_mouse.focused = event->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
            g_mouse.dx = g_mouse.dy = g_mouse.wheel = 0;
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
        {
            // A finger or a pen moving the cursor is not the mouse.
            if (event->motion.which == SDL_TOUCH_MOUSEID || event->motion.which == SDL_PEN_MOUSEID) break;
            // An event's place is in the window's own units, the frame's in pixels.
            const float density = g.window ? SDL_GetWindowPixelDensity(g.window) : 1.0f;
            std::lock_guard lock(g_mouse.lock);
            if (g_mouse.focused && g_mouse.held)
            {
                g_mouse.dx += event->motion.xrel;
                g_mouse.dy += event->motion.yrel;
            }
            else
            {
                g_mouse.x = event->motion.x * density;
                g_mouse.y = event->motion.y * density;
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
        {
            std::lock_guard lock(g_mouse.lock);
            if (g_mouse.focused && !g_mouse.freed)
                g_mouse.wheel += event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event->wheel.y : event->wheel.y;
            break;
        }
        default:
            break;
        }
        return true;
    }

    // Takes the mouse or lets it go, as F2 and the title's menus have it: held
    // unless the player has the cursor back or a menu is using it to point
    // with. On the window's thread, every time round its loop.
    void HoldMouse()
    {
        bool hold;
        {
            std::lock_guard lock(g_mouse.lock);
            hold = !g_mouse.freed && !g_mouse.pointing;
            if (hold == g_mouse.held) return;
            g_mouse.held = hold;
            g_mouse.dx = g_mouse.dy = 0;
        }
        if (!SDL_SetWindowRelativeMouseMode(g.window, hold))
            LOGW("vulkan: the mouse would not be %s (%s)", hold ? "held" : "let go", SDL_GetError());
    }

    // The cursor given back to the player, or taken again. On the window's
    // thread.
    void FreeMouse(bool freed)
    {
        {
            std::lock_guard lock(g_mouse.lock);
            g_mouse.freed = freed;
        }
        HoldMouse();
    }
#endif

    bool Check(VkResult r, const char* what)
    {
        if (r == VK_SUCCESS) return true;
        LOGW("vulkan: %s failed (%d)", what, int(r));
        return false;
    }

    // Under VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation, what the layer
    // reports goes into this log. Left to itself it prints to stdout, which
    // is block-buffered into a file and never flushed at this runtime's exit:
    // a run with a hundred errors read as a clean one.
    std::atomic<uint64_t> g_validationMessages{ 0 };
    VkDebugUtilsMessengerEXT g_messenger = VK_NULL_HANDLE;

    VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*)
    {
        const uint64_t n = g_validationMessages.fetch_add(1, std::memory_order_relaxed);
        if (n < 500)
            LOGW("validation %s: %s",
                 severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
                 data && data->pMessage ? data->pMessage : "?");
        return VK_FALSE;
    }

    bool Validating()
    {
        const char* layers = std::getenv("VK_INSTANCE_LAYERS");
        return layers && std::strstr(layers, "validation");
    }

    // Whether the installed loader/driver offers an instance-level extension
    // (VK_KHR_get_physical_device_properties2, VK_KHR_maintenance1 ...).
    bool InstanceHasExtension(const char* name)
    {
        uint32_t count = 0;
        if (vkEnumerateInstanceExtensionProperties(name, &count, nullptr) != VK_SUCCESS || !count)
            return false;
        std::vector<VkExtensionProperties> offered(count);
        if (vkEnumerateInstanceExtensionProperties(name, &count, offered.data()) != VK_SUCCESS)
            return false;
        for (const auto& e : offered)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    }

    bool CreateInstance()
    {
        std::vector<const char*> extensions;
#ifdef MW2_ANDROID
        // Android has one window system and it is always there.
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        extensions.push_back("VK_KHR_android_surface");
#else
        uint32_t extensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
        if (!sdlExtensions) { LOGW("vulkan: SDL has no instance extensions (%s)", SDL_GetError()); return false; }
        extensions.assign(sdlExtensions, sdlExtensions + extensionCount);
#endif

        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "mw2recomp";
        // 1.0 is the floor: negative viewport heights (VK_KHR_maintenance1
        // before that) are an extension too, and 1.0 is the version the APK
        // advertises on its Play-store device filter. Everything newer is
        // optional and asked for by extension or checked at device time.
        // The highest the loader will admit to, capped at 1.3. It matters:
        // dynamic rendering and extended dynamic state are core there, and
        // a driver that has them in core rather than as extensions can only
        // be asked for them by an application that says it targets 1.3.
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
                else if (available >= VK_API_VERSION_1_1)
                    app.apiVersion = VK_API_VERSION_1_1;
            }
        }

        // The loader would name these itself on Android; everywhere else they
        // come from SDL's list. A 1.0 instance has to name what 1.1 and 1.2
        // make core, because the runtime asks for maintenance1's negative
        // viewport and for `vkGetPhysicalDeviceProperties2` / `...Features2`
        // (properties2's pNext chain, the depth resolve and driver
        // properties read here and in pipeline.cpp) and those are pre-1.1
        // extensions behind both.
        if (app.apiVersion < VK_API_VERSION_1_1)
        {
            if (vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties") &&
                InstanceHasExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
                extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            if (InstanceHasExtension(VK_KHR_MAINTENANCE1_EXTENSION_NAME))
                extensions.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
        }

        VkDebugUtilsMessengerCreateInfoEXT messenger{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        messenger.pfnUserCallback = Validation;
        const bool validating = Validating();
        if (validating) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        VkInstanceCreateInfo info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        info.pNext = validating ? &messenger : nullptr;   // instance creation's own messages
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = uint32_t(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        if (!Check(vkCreateInstance(&info, nullptr, &g.instance), "vkCreateInstance")) return false;
#if defined(MW2_VULKAN_DYNAMIC)
        // Everything past this point is called through the driver's own
        // entry points rather than a link-time symbol (loader.h).
        vk::loader::LoadInstance(g.instance);
#endif
        if (validating)
        {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(g.instance, "vkCreateDebugUtilsMessengerEXT"));
            if (!create || create(g.instance, &messenger, nullptr, &g_messenger) != VK_SUCCESS)
                LOGW("vulkan: validation asked for, but its messages cannot be caught");
            else
                LOGI("vulkan: validation messages go to this log");
        }
        return true;
    }

    bool PickDevice()
    {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(g.instance, &count, nullptr);
        if (!count) { LOGW("vulkan: no physical devices"); return false; }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(g.instance, &count, devices.data());

        // Prefer a discrete device, but take whatever can present.
        VkPhysicalDevice fallback = VK_NULL_HANDLE;
        uint32_t fallbackFamily = 0;
        for (VkPhysicalDevice candidate : devices)
        {
            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> properties(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, properties.data());

            for (uint32_t i = 0; i < families; i++)
            {
                if (!(properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                VkBool32 canPresent = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, g.surface, &canPresent);
                if (!canPresent) continue;

                VkPhysicalDeviceProperties info{};
                vkGetPhysicalDeviceProperties(candidate, &info);
                if (info.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                {
                    g.physical = candidate; g.queueFamily = i;
                    LOGI("vulkan: %s", info.deviceName);
                    return true;
                }
                if (!fallback) { fallback = candidate; fallbackFamily = i; }
                break;
            }
        }
        if (!fallback) { LOGW("vulkan: no device can present to the window"); return false; }
        g.physical = fallback; g.queueFamily = fallbackFamily;
        VkPhysicalDeviceProperties info{};
        vkGetPhysicalDeviceProperties(g.physical, &info);
        LOGI("vulkan: %s", info.deviceName);
        return true;
    }

    // The renderer draws on this device when there is a window, so it is made
    // the way pipeline.cpp makes its own, with the swapchain beside.
    bool CreateDevice()
    {
        std::vector<const char*> extensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        // Present ids and waiting on them say when a frame reached the
        // screen, which is the display's clock.
        VkPhysicalDevicePresentIdFeaturesKHR presentId{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
        VkPhysicalDevicePresentWaitFeaturesKHR presentWait{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR };
        void* features = nullptr;
        if (vk::pipeline::DeviceHasExtension(g.physical, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
            vk::pipeline::DeviceHasExtension(g.physical, VK_KHR_PRESENT_WAIT_EXTENSION_NAME))
        {
            presentId.pNext = &presentWait;
            VkPhysicalDeviceFeatures2 query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            query.pNext = &presentId;
            vkGetPhysicalDeviceFeatures2(g.physical, &query);
            if (presentId.presentId && presentWait.presentWait)
            {
                extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
                extensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
                presentWait.pNext = nullptr;
                features = &presentId;
                g.presentWait = true;
            }
        }
        void* device = nullptr;
        void* queue = nullptr;
        if (!vk::pipeline::CreateDevice(g.physical, g.queueFamily, extensions.data(),
                                        uint32_t(extensions.size()), &device, &queue, features))
            return false;
        g.device = static_cast<VkDevice>(device);
        g.queue = static_cast<VkQueue>(queue);
        if (g.presentWait)
        {
            g.waitForPresent = reinterpret_cast<PFN_vkWaitForPresentKHR>(
                vkGetDeviceProcAddr(g.device, "vkWaitForPresentKHR"));
            g.presentWait = g.waitForPresent != nullptr;
        }
        if (!g.presentWait)
            LOGW("vulkan: the device cannot say when a present reaches the screen"
                 " (VK_KHR_present_wait); the guest's blank keeps its own 60 Hz clock");
        return true;
    }

    bool CreateSwapchain()
    {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical, g.surface, &caps);
        // The surface says what size it is, except where the window system
        // leaves it to the application (both dimensions at the maximum), and
        // then the window's own size in pixels is the answer, held within
        // what the surface allows.
        if (caps.currentExtent.width != UINT32_MAX) g.extent = caps.currentExtent;
        else
        {
            int width = 0, height = 0;
#ifdef MW2_ANDROID
            if (g.nativeWindow)
            {
                width = ANativeWindow_getWidth(g.nativeWindow);
                height = ANativeWindow_getHeight(g.nativeWindow);
            }
#else
            SDL_GetWindowSizeInPixels(g.window, &width, &height);
#endif
            g.extent.width = std::clamp(uint32_t(std::max(width, 0)),
                                        caps.minImageExtent.width, caps.maxImageExtent.width);
            g.extent.height = std::clamp(uint32_t(std::max(height, 0)),
                                         caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (!g.extent.width || !g.extent.height) return false;   // minimised: nothing to make

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical, g.surface, &formatCount, nullptr);
        if (!formatCount) return false;
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical, g.surface, &formatCount, formats.data());
        VkSurfaceFormatKHR chosen = formats[0];
        for (const auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = f; break; }
        g.swapchainFormat = chosen.format;

        VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        info.surface = g.surface;
#ifdef MW2_ANDROID
        info.minImageCount = std::max(caps.minImageCount, 3u);
#else
        info.minImageCount = std::max(caps.minImageCount, 2u);
#endif
        if (caps.maxImageCount && info.minImageCount > caps.maxImageCount) info.minImageCount = caps.maxImageCount;
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = g.extent;
        info.imageArrayLayers = 1;
        // The renderer's frame is blitted in, so a transfer destination.
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // How the frame sits on the panel.
        //
        // A phone's panel is wired portrait, so a landscape window reports
        // currentTransform as ROTATE_90. Passing that back says "my frame is
        // already turned to match" -- and this renderer's is not, it draws
        // the console's landscape frame square on. The result is a picture
        // lying on its side, which is what the first run on a phone showed.
        //
        // Identity instead, where the surface allows it: the compositor then
        // turns the frame, which it is going to be doing for the system bars
        // anyway. It costs a pass the pre-rotated path would not, and the
        // alternative is rotating every frame in the blit below -- worth
        // doing one day, not worth an upside-down menu today.
        info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                : caps.currentTransform;
        g.suboptimalIsExpected = info.preTransform != caps.currentTransform;
        // Once. This is remade on every rotation and every resize, and a line
        // a frame is its own performance problem.
        static bool toldAboutTheTransform = false;
        if (g.suboptimalIsExpected && !std::exchange(toldAboutTheTransform, true))
            LOGI("vulkan: the panel is mounted %s; the compositor turns the frame",
                 caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR  ? "a quarter turn"
                 : caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? "upside down"
                 : caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ? "three quarters round"
                                                                                    : "at some angle");
        else if (caps.currentTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
            LOGW("vulkan: the surface will not take an identity transform; the picture may be"
                 " rotated");
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;      // always supported
        info.clipped = VK_TRUE;
        // The one being replaced, if any, so the driver can carry over what it
        // can; it is destroyed once the new one exists.
        VkSwapchainKHR old = g.swapchain;
        info.oldSwapchain = old;

        VkSwapchainKHR made = VK_NULL_HANDLE;
        const bool ok = Check(vkCreateSwapchainKHR(g.device, &info, nullptr, &made),
                              "vkCreateSwapchainKHR");
        {
            // Nothing waits on a present of the old one once it is gone.
            std::unique_lock life(g.swapchainLife);
            // The sharpening's views of the old one's images go before them.
            vk::sharpen::ForgetTargets();
            if (old) vkDestroySwapchainKHR(g.device, old, nullptr);
            g.swapchain = ok ? made : VK_NULL_HANDLE;
            std::lock_guard waiting(g.waitLock);
            g.waiting.clear();
            g.lastShownNs = -1;
        }
        if (!ok) return false;
        g.swapchainsMade++;

        uint32_t imageCount = 0;
        vkGetSwapchainImagesKHR(g.device, g.swapchain, &imageCount, nullptr);
        g.images.resize(imageCount);
        vkGetSwapchainImagesKHR(g.device, g.swapchain, &imageCount, g.images.data());
        LOGI("vulkan: swapchain %ux%u, %u images", g.extent.width, g.extent.height, imageCount);
        return true;
    }

    bool CreateFrames()
    {
        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = g.queueFamily;
        if (!Check(vkCreateCommandPool(g.device, &poolInfo, nullptr, &g.pool), "vkCreateCommandPool"))
            return false;

        for (auto& frame : g.frames)
        {
            VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocate.commandPool = g.pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            if (!Check(vkAllocateCommandBuffers(g.device, &allocate, &frame.commands), "vkAllocateCommandBuffers"))
                return false;

            VkSemaphoreCreateInfo semaphore{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            VkFenceCreateInfo fence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (!Check(vkCreateSemaphore(g.device, &semaphore, nullptr, &frame.acquired), "vkCreateSemaphore") ||
                !Check(vkCreateSemaphore(g.device, &semaphore, nullptr, &frame.rendered), "vkCreateSemaphore") ||
                !Check(vkCreateFence(g.device, &fence, nullptr, &frame.inFlight), "vkCreateFence"))
                return false;
        }
        return true;
    }

    // Only the presenter's own command buffers touch the swapchain images, so
    // waiting for its two fences is enough to replace it; the renderer's queue
    // work carries on. A window without a size keeps the old swapchain, which
    // stays unusable, and every frame until it has one is skipped.
    void RemakeSwapchain()
    {
        VkFence fences[kFramesInFlight];
        for (uint32_t i = 0; i < kFramesInFlight; i++) fences[i] = g.frames[i].inFlight;
        vk::pipeline::Failed(vkWaitForFences(g.device, kFramesInFlight, fences, VK_TRUE, UINT64_MAX),
                             "the presenter's fences before a resize");
        if (CreateSwapchain()) g.remakeSwapchain = false;
    }

    // Where the frame lands in a window of another shape: as large as fits
    // at the frame's own proportions, centred, the rest black. Nothing is
    // squashed, and the window can be any shape at all. Under MW2_STRETCH=1
    // it fills the window whatever the shapes: a CRT TV fed a widescreen
    // signal shows a 4:3 picture made as wide as that at its own shape.
    void Letterbox(uint32_t frameWidth, uint32_t frameHeight, VkOffset3D& from, VkOffset3D& to)
    {
        if (g.stretch)
        {
            from = { 0, 0, 0 };
            to = { int32_t(g.extent.width), int32_t(g.extent.height), 1 };
            return;
        }
        const uint64_t byWidth = uint64_t(g.extent.width) * frameHeight;
        const uint64_t byHeight = uint64_t(g.extent.height) * frameWidth;
        uint32_t width = g.extent.width, height = g.extent.height;
        if (byWidth > byHeight) width = uint32_t(byHeight / frameHeight);   // wider than the frame
        else height = uint32_t(byWidth / frameWidth);                       // taller than the frame
        width = std::max(width, 1u);
        height = std::max(height, 1u);
        from = { int32_t((g.extent.width - width) / 2), int32_t((g.extent.height - height) / 2), 0 };
        to = { from.x + int32_t(width), from.y + int32_t(height), 1 };
    }

    void Waiter();

    // The display's rate as its mode gives it; 60 when nothing says.
    double ModeRate()
    {
        double hz = 0;
#ifdef MW2_ANDROID
        // Display.getRefreshRate, as the app reported it, at the surface's
        // making and whenever the display's mode changes after it.
        hz = android::RefreshHz();
        g.exactRate = false;
#else
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g.window)))
        {
            // The mode's exact rate, from its pixel clock and totals, when
            // the window system gives it; otherwise measured (SawDisplayed).
            g.exactRate = mode->refresh_rate_denominator != 0;
            hz = g.exactRate ? double(mode->refresh_rate_numerator) / mode->refresh_rate_denominator
                             : double(mode->refresh_rate);
        }
#endif
        return hz > 0 ? hz : 60.0;
    }

    // The display runs at `hz` from now on (under waitLock once the waiter
    // runs). The guest's blank follows the display when the display runs at
    // the console's 60 Hz and the device can say when a present reaches it. A
    // display at another rate cannot show 60 frames a second evenly anyway,
    // and the title keeps its own 60; what paces its frames there is
    // frame_rate.h's to choose, from the rate given it here.
    void AdoptDisplayRate(double hz)
    {
        g.modeHz = hz;
        g.periodNs = 1e9 / hz;
        g.measured = 0;
        g.lastShownNs = -1;
        frame_rate::SetDisplay(hz, g.periodNs);
        const bool followed = g.presentWait && hz >= 59.0 && hz <= 61.0;
        g.nominalNs = followed ? g.periodNs : 0;
        if (!followed) g.guestPeriodNs.store(0, std::memory_order_relaxed);
        LOGI("vulkan: the display runs at %.4f Hz; the guest's blank %s", hz,
             followed ? "follows it" : "keeps its own 60 Hz clock");
    }

    void FollowTheDisplay()
    {
        AdoptDisplayRate(ModeRate());
        if (g.presentWait) g.waiter = std::thread(Waiter);
    }

#ifdef MW2_ANDROID
    // A phone changes its display's mode under a running game: the system
    // grants the rate the app asked for a moment after the surface is made,
    // and takes it back when the battery or the heat says so.
    void FollowModeChange()
    {
        const double hz = ModeRate();
        if (std::abs(hz - g.modeHz) < 0.5) return;
        std::lock_guard lock(g.waitLock);
        AdoptDisplayRate(hz);
    }
#endif

    // How much longer the guest's blank may be than the display's while
    // latency drains: 0.5%, a frame in about three seconds, and nobody sees it.
    constexpr double kNudge = 0.005;

    int64_t Nanoseconds()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Frame `id` reached the screen at `at` (under waitLock). A frame held on
    // screen longer than a blank is a stutter; the guest's blank is set to the
    // display's, a touch longer while more than one frame waits behind the
    // one on screen, so latency a stall built up drains away unseen. It is
    // never set shorter: a frame late once leaves one frame in hand from then
    // on, which is double buffering, and when the renderer cannot keep up,
    // hurrying the title only gives it more to fall behind on.
    //
    // The display's period comes from its mode where the mode says it
    // exactly. Measured from these completions it read 17.1 ms against the
    // panel's 16.67 in a run where frames were often late -- late completions
    // pull an average up -- and the guest's blank followed it down to 58.5 Hz.
    // On the console the blank the title counts is the display's own; a
    // timer of ours beside it drifts, and every so often a frame is shown
    // twice or waits a frame longer.
    void SawDisplayed(uint64_t id, int64_t at)
    {
        const uint64_t queued = g.lastQueued.load(std::memory_order_relaxed);
        // Under the title's limiter nothing waits its turn behind the frame
        // on screen: the frames after it are let go for the newest
        // (DrawOneFrame).
        const double behind = frame_rate::Mode() != frame_rate::kByLimiter && queued > id
                                  ? double(queued - id) : 0.0;
        if (g.lastShownNs >= 0 && id > g.lastShownId)
        {
            const double gap = double(at - g.lastShownNs);
            const uint64_t frames = id - g.lastShownId;
            const long blanks = std::lround(gap / g.periodNs);
            // Only a gap no longer than the period can be trusted to measure it.
            if (!g.exactRate && frames == 1 && gap <= g.periodNs * 1.02 && gap > g.periodNs * 0.9)
            {
                g.periodNs += (gap - g.periodNs) / 256.0;
                frame_rate::SetDisplay(g.modeHz, g.periodNs);
                // Four times the average's length: what the mode's rate was
                // out by has gone from it.
                if (++g.measured == 1024)
                    LOGI("vulkan: the display's blank measures %.1f us, %.3f Hz, against %.3f Hz"
                         " from its mode", g.periodNs / 1000.0, 1e9 / g.periodNs, g.modeHz);
            }
            // A frame is due to stay as many blanks as the limit makes it
            // (60 on a 120 Hz display: two).
            const long due = long(frames) * long(frame_rate::BlanksAFrame());
            if (blanks > due) stutters::Displayed(id, uint32_t(blanks - due), gap / 1e6);
        }
        g.lastShownNs = at;
        g.lastShownId = id;
        g.buffered += (behind - g.buffered) / 16.0;
        g.behindCount[std::min<uint64_t>(uint64_t(behind), 3)]++;
        if (!g.nominalNs) return;
        const double stretch = g.buffered > 1.5 ? kNudge : 0.0;
        (stretch > 0 ? g.stretched : g.shownOnTime)++;
        g.guestPeriodNs.store(uint64_t(g.periodNs * (1.0 + stretch)), std::memory_order_relaxed);
        g.guestPeriodAt.store(at, std::memory_order_relaxed);
    }

    // Waits on each present in turn until it is on the screen. Its own thread,
    // as DXVK does it: the window thread cannot wait on the display without
    // falling behind the frames it has to show.
    void Waiter()
    {
        crash::RegisterThread("vulkan present wait");
        while (g.running.load(std::memory_order_relaxed))
        {
            uint64_t id = 0;
            {
                std::unique_lock lock(g.waitLock);
                g.waitWake.wait_for(lock, std::chrono::milliseconds(50),
                                    [] { return !g.waiting.empty() || !g.running; });
                if (g.waiting.empty()) continue;
                id = g.waiting.front();
            }
            VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
            {
                std::shared_lock life(g.swapchainLife);
                if (g.swapchain)
                    r = g.waitForPresent(g.device, g.swapchain, id, 20'000'000);   // 20 ms
            }
            if (r == VK_TIMEOUT) continue;   // still waiting; lets a resize in
            const int64_t at = Nanoseconds();
            std::lock_guard lock(g.waitLock);
            // A resize cleared the list, and this id went with the old swapchain.
            if (g.waiting.empty() || g.waiting.front() != id) continue;
            g.waiting.pop_front();
            if (r == VK_SUCCESS) SawDisplayed(id, at);
            else g.lastShownNs = -1;
        }
    }

    // The frames queued up to now are let go without being shown: there is
    // no swapchain to show them in (a minimised window), and the renderer must
    // not wait on a window that cannot take them.
    void LetGo()
    {
        std::lock_guard lock(g.lock);
        if (g.holdsLeft) g.taken = std::max(g.taken, g.held.serial);
        g.holdsLeft = 0;
        if (g.frameQueue.empty()) { g.takenWake.notify_all(); return; }
        g.taken = std::max(g.taken, g.frameQueue.back().serial);
        g.frameQueue.clear();
        g.takenWake.notify_all();
    }

    // Shows the oldest queued frame, or with `idle` the window's own colour
    // before the renderer has finished any.
    void DestroyOverlay()
    {
        auto& o = g.overlay;
        if (o.view) vkDestroyImageView(g.device, o.view, nullptr);
        if (o.image) vkDestroyImage(g.device, o.image, nullptr);
        if (o.imageMemory) vkFreeMemory(g.device, o.imageMemory, nullptr);
        if (o.staging) vkDestroyBuffer(g.device, o.staging, nullptr);
        if (o.stagingMemory) vkFreeMemory(g.device, o.stagingMemory, nullptr);
        o.image = VK_NULL_HANDLE; o.imageMemory = VK_NULL_HANDLE; o.view = VK_NULL_HANDLE;
        o.staging = VK_NULL_HANDLE; o.stagingMemory = VK_NULL_HANDLE;
        o.mapped = nullptr; o.width = o.height = 0; o.everUploaded = false;
    }

    bool CreateOverlay(uint32_t width, uint32_t height)
    {
        auto& o = g.overlay;
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(g.physical, &memory);

        VkImageCreateInfo image{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_B8G8R8A8_UNORM;
        image.extent = { width, height, 1 };
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(vkCreateImage(g.device, &image, nullptr, &o.image), "the screens' image")) return false;
        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(g.device, o.image, &needs);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = vk::util::FindMemory(memory, needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocate.memoryTypeIndex == UINT32_MAX ||
            !Check(vkAllocateMemory(g.device, &allocate, nullptr, &o.imageMemory), "the sign-in screen's memory") ||
            !Check(vkBindImageMemory(g.device, o.image, o.imageMemory, 0), "the sign-in screen's memory"))
            return false;
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = o.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (!Check(vkCreateImageView(g.device, &view, nullptr, &o.view), "the screens' view")) return false;

        VkBufferCreateInfo buffer{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer.size = VkDeviceSize(width) * height * 4;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!Check(vkCreateBuffer(g.device, &buffer, nullptr, &o.staging), "the sign-in screen's buffer")) return false;
        vkGetBufferMemoryRequirements(g.device, o.staging, &needs);
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = vk::util::FindMemory(memory, needs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (allocate.memoryTypeIndex == UINT32_MAX ||
            !Check(vkAllocateMemory(g.device, &allocate, nullptr, &o.stagingMemory), "the sign-in screen's buffer") ||
            !Check(vkBindBufferMemory(g.device, o.staging, o.stagingMemory, 0), "the sign-in screen's buffer") ||
            !Check(vkMapMemory(g.device, o.stagingMemory, 0, VK_WHOLE_SIZE, 0, &o.mapped), "the sign-in screen's buffer"))
            return false;
        o.width = width;
        o.height = height;
        return true;
    }

    // The screen open over the middle of the swapchain image, which is in
    // TRANSFER_DST layout: over a blur of the game, faded in and out
    // (backdrop.h), or else copied over as it is.
    void DrawOverlay(const Frame& frame, VkImage target, const Presenter::Queued& shown,
                     VkOffset3D frameFrom, VkOffset3D frameTo)
    {
        auto& o = g.overlay;
        const screen::Image* picture = signin::Picture(g.extent.height, o.signin)    ? &o.signin
                                     : friends::Picture(g.extent.height, o.friends) ? &o.friends
                                     : guide::Picture(g.extent.height, o.guide)     ? &o.guide
                                                                                     : nullptr;
        const bool layered = vk::backdrop::Create(g.device, g.physical, g.swapchainFormat);
        // The fade goes on after the screen shuts, with the picture it last had.
        const auto now = std::chrono::steady_clock::now();
        const float step = std::min(std::chrono::duration<float>(now - o.drawn).count(), 1.0f / 30) / kFade;
        o.drawn = now;
        o.fade = layered ? std::clamp(o.fade + (picture ? step : -step), 0.0f, 1.0f) : picture ? 1.0f : 0.0f;
        if (!picture && (o.fade <= 0 || !o.everUploaded)) return;

        if (picture)
        {
            const uint32_t width = picture->width, height = picture->height;
            if (width > g.extent.width || height > g.extent.height) return;
            const bool resized = width != o.width || height != o.height;
            if (resized || o.shown != picture || o.uploaded != picture->version || !o.everUploaded ||
                o.layered != layered)
            {
                // The other frames' commands may still be reading the image and
                // the buffer; this happens at a press of a button, not per frame.
                for (const Frame& other : g.frames)
                    if (&other != &frame) vkWaitForFences(g.device, 1, &other.inFlight, VK_TRUE, UINT64_MAX);
                if (resized)
                {
                    DestroyOverlay();
                    if (!CreateOverlay(width, height)) { DestroyOverlay(); return; }
                }
                std::memcpy(o.mapped, picture->pixels.data(), picture->pixels.size());
                vk::util::Barrier(frame.commands, o.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy copy{};
                copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                copy.imageExtent = { width, height, 1 };
                vkCmdCopyBufferToImage(frame.commands, o.staging, o.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                vk::util::Barrier(frame.commands, o.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  layered ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                          : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_ACCESS_TRANSFER_WRITE_BIT,
                                  layered ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  layered ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT);
                o.uploaded = picture->version;
                o.shown = picture;
                o.outline = picture->outline;
                o.layered = layered;
                o.everUploaded = true;
            }
        }
        const int32_t left = int32_t(g.extent.width - o.width) / 2, top = int32_t(g.extent.height - o.height) / 2;

        // A fade out with no picture any more can only go on as it was laid out.
        if (o.layered != layered) return;
        if (shown.image) { o.frameFrom = frameFrom; o.frameTo = frameTo; }

        if (layered)
        {
            vk::backdrop::Draw draw{};
            draw.commands = frame.commands;
            draw.slot = g.frameIndex;
            draw.target = target;
            draw.extent = g.extent;
            draw.frame = shown.image;
            draw.frameWidth = shown.width;
            draw.frameHeight = shown.height;
            draw.frameFrom = { o.frameFrom.x, o.frameFrom.y };
            draw.frameTo = { o.frameTo.x, o.frameTo.y };
            draw.picture = o.view;
            draw.pictureFrom = { left, top };
            draw.pictureTo = { left + int32_t(o.width), top + int32_t(o.height) };
            draw.outline = o.outline;
            // Eased, so it settles rather than stops.
            draw.shown = 1.0f - (1.0f - o.fade) * (1.0f - o.fade);
            draw.settle = [&] {
                for (const Frame& other : g.frames)
                    if (&other != &frame) vkWaitForFences(g.device, 1, &other.inFlight, VK_TRUE, UINT64_MAX);
            };
            vk::backdrop::Record(draw);
            return;
        }

        // The frame's blit wrote where this one writes.
        vk::util::Barrier(frame.commands, target, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { int32_t(o.width), int32_t(o.height), 1 };
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0] = { left, top, 0 };
        blit.dstOffsets[1] = { left + int32_t(o.width), top + int32_t(o.height), 1 };
        vkCmdBlitImage(frame.commands, o.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }

    void DrawOneFrame(bool idle)
    {
        if (g.remakeSwapchain) RemakeSwapchain();
        if (g.remakeSwapchain || !g.swapchain) { LetGo(); return; }

        Frame& frame = g.frames[g.frameIndex];
        if (vk::pipeline::Failed(vkWaitForFences(g.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX),
                                 "the presenter's fence"))
            return;

        // FIFO: this is where the window waits for the display, with the
        // frame it will show still in the queue, so the queue's length is
        // how far the renderer is ahead of the screen.
        uint32_t imageIndex = 0;
        VkResult acquired = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX,
                                                  frame.acquired, VK_NULL_HANDLE, &imageIndex);
        // Out of date: nothing was acquired, and the semaphore is untouched;
        // remade next frame. Suboptimal: an image was acquired and is shown,
        // since the semaphore it signals has to be waited on, and then remade.
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) { g.remakeSwapchain = true; return; }
        if (acquired == VK_SUBOPTIMAL_KHR) { if (!g.suboptimalIsExpected) g.remakeSwapchain = true; }
        else if (acquired != VK_SUCCESS) return;

        Presenter::Queued shown;
        // Shown before, and on screen for another blank.
        bool again = false;
        if (!idle)
        {
            std::lock_guard lock(g.lock);
            if (g.holdsLeft)
            {
                shown = g.held;
                g.holdsLeft--;
                again = true;
            }
            else
            {
                // Under the title's limiter (frame_rate.h) the title finishes
                // frames whenever it likes, and the display still takes one a
                // blank: the newest is shown and the ones before it are let go.
                // Paced by the console's blank or the display's, every frame is
                // shown in its turn.
                while (frame_rate::Mode() == frame_rate::kByLimiter && g.frameQueue.size() > 1)
                {
                    g.taken = std::max(g.taken, g.frameQueue.front().serial);
                    g.frameQueue.pop_front();
                    g.takenWake.notify_all();
                }
                if (!g.frameQueue.empty())
                {
                    shown = g.frameQueue.front();
                    g.frameQueue.pop_front();
                    if (frame_rate::ConsoleBlanks() == 2)
                    {
                        g.held = shown;
                        g.holdsLeft = frame_rate::BlanksAFrame() - 1;
                    }
                }
            }
        }

        vkResetFences(g.device, 1, &frame.inFlight);
        vkResetCommandBuffer(frame.commands, 0);

        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(frame.commands, &begin);

        VkImage image = g.images[imageIndex];
        // At the transfer stage, which the acquire's semaphore holds back, so the
        // transition does not run before the image is ours.
        vk::util::Barrier(frame.commands, image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkOffset3D frameFrom{}, frameTo{};
        if (shown.image && shown.width && shown.height)
        {
            // The renderer's frame, scaled to the window at its own proportions.
            // It is in TRANSFER_SRC layout, and its owner does not draw into it
            // again until this copy is submitted (WaitUntilTaken).
            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[1] = { int32_t(shown.width), int32_t(shown.height), 1 };
            blit.dstSubresource = blit.srcSubresource;
            Letterbox(shown.width, shown.height, blit.dstOffsets[0], blit.dstOffsets[1]);
            frameFrom = blit.dstOffsets[0];
            frameTo = blit.dstOffsets[1];
#ifdef _WIN32
            {
                // Where a menu's pointer is on the frame (TakeMouse).
                std::lock_guard lock(g_mouse.lock);
                g_mouse.picture[0] = frameFrom.x;
                g_mouse.picture[1] = frameFrom.y;
                g_mouse.picture[2] = frameTo.x;
                g_mouse.picture[3] = frameTo.y;
            }
#endif
            // Sharpened into its place (sharpen.h), or else copied: the bars
            // are cleared first; the clear covers the whole image and the blit
            // paints over the middle, which is cheaper than working out the bars.
            vk::sharpen::Draw sharpened{};
            sharpened.device = g.device;
            sharpened.commands = frame.commands;
            sharpened.slot = g.frameIndex;
            sharpened.target = image;
            sharpened.targetFormat = g.swapchainFormat;
            sharpened.extent = g.extent;
            sharpened.frame = shown.image;
            sharpened.from = { frameFrom.x, frameFrom.y };
            sharpened.to = { frameTo.x, frameTo.y };
            if (!vk::sharpen::Record(sharpened))
            {
                const bool bars = blit.dstOffsets[0].x || blit.dstOffsets[0].y;
                if (bars)
                {
                    VkClearColorValue black{};
                    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    vkCmdClearColorImage(frame.commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         &black, 1, &range);
                    vk::util::Barrier(frame.commands, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                }
                vkCmdBlitImage(frame.commands, shown.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            }
        }
        else
        {
            // A recognisably "nothing rendered yet" colour rather than black, so
            // a blank window is distinguishable from a dead one.
            VkClearColorValue colour{};
            colour.float32[0] = 0.06f; colour.float32[1] = 0.07f; colour.float32[2] = 0.10f; colour.float32[3] = 1.0f;
            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(frame.commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
        }
        DrawOverlay(frame, image, shown, frameFrom, frameTo);

        vk::util::Barrier(frame.commands, image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(frame.commands);

        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.acquired;
        submit.pWaitDstStageMask = &wait;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.commands;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &frame.rendered;
        // The GPU thread submits to this queue too, and Vulkan requires a queue
        // to be externally synchronised for a present as for a submission. Two
        // holds rather than one, so a submission of the renderer's can go
        // between them.
        VkResult submitRes = VK_SUCCESS;
        {
            std::lock_guard queueLock(vk::pipeline::QueueMutex());
            submitRes = vk::gpu_time::Submit(g.queue, submit, frame.inFlight, vk::gpu_time::Source::kPresent);
        }
        if (submitRes != VK_SUCCESS)
        {
            vk::pipeline::Failed(submitRes, "the presenter's submission");
            VkSubmitInfo emptySubmit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            std::lock_guard queueLock(vk::pipeline::QueueMutex());
            vkQueueSubmit(g.queue, 1, &emptySubmit, frame.inFlight);
            return;
        }
        // The copy is in the queue ahead of anything the renderer submits from
        // now on, so the renderer may draw over the image, unless it is to be
        // copied again for the next blank.
        if (shown.serial)
        {
            std::lock_guard lock(g.lock);
            if (!g.holdsLeft)
            {
                g.taken = std::max(g.taken, shown.serial);
                g.takenWake.notify_all();
            }
        }
        if (shown.serial && !again) stutters::Shown(shown.serial);

        VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &frame.rendered;
        present.swapchainCount = 1;
        present.pSwapchains = &g.swapchain;
        present.pImageIndices = &imageIndex;
        // The frame's serial is its present id: they only ever go up.
        VkPresentIdKHR id{ VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
        id.swapchainCount = 1;
        id.pPresentIds = &shown.serial;
        if (g.presentWait && shown.serial && !again) present.pNext = &id;
        // Out of date and suboptimal here mean the window changed under the
        // present: the swapchain is remade before the next frame. Only a lost
        // device is a failure.
        const VkResult presented = [&] {
            std::lock_guard queueLock(vk::pipeline::QueueMutex());
            return vkQueuePresentKHR(g.queue, &present);
        }();
        if (presented == VK_ERROR_OUT_OF_DATE_KHR ||
            (presented == VK_SUBOPTIMAL_KHR && !g.suboptimalIsExpected))
            g.remakeSwapchain = true;
        else
            vk::pipeline::Failed(presented, "present");
        if (g.presentWait && shown.serial && !again &&
            (presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR))
        {
            std::lock_guard waiting(g.waitLock);
            g.waiting.push_back(shown.serial);
            g.waitWake.notify_one();
        }

        g.frameIndex = (g.frameIndex + 1) % kFramesInFlight;
        // Frames, not blanks: the app's frame counter reads this.
        if (!again) g.presented.fetch_add(1, std::memory_order_relaxed);
    }

    void Teardown()
    {
        if (g.device)
        {
            vkDeviceWaitIdle(g.device);
            vk::backdrop::Destroy();
            vk::sharpen::Destroy();
            DestroyOverlay();
            for (auto& frame : g.frames)
            {
                if (frame.acquired) vkDestroySemaphore(g.device, frame.acquired, nullptr);
                if (frame.rendered) vkDestroySemaphore(g.device, frame.rendered, nullptr);
                if (frame.inFlight) vkDestroyFence(g.device, frame.inFlight, nullptr);
            }
            if (g.pool) vkDestroyCommandPool(g.device, g.pool, nullptr);
            if (g.swapchain) vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
#if defined(MW2_VULKAN_DYNAMIC)
            vk::loader::ForgetDevice();
#endif
            vkDestroyDevice(g.device, nullptr);
            g.pool = VK_NULL_HANDLE;
            g.swapchain = VK_NULL_HANDLE;
            g.device = VK_NULL_HANDLE;
            for (auto& frame : g.frames) frame = Frame{};
        }
        if (g.surface) vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
        if (g_messenger)
        {
            LOGI("vulkan: %llu validation messages", (unsigned long long)g_validationMessages.load());
            if (auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(g.instance, "vkDestroyDebugUtilsMessengerEXT")))
                destroy(g.instance, g_messenger, nullptr);
            g_messenger = VK_NULL_HANDLE;
        }
        if (g.instance) vkDestroyInstance(g.instance, nullptr);
#ifdef MW2_USE_SDL
#ifdef _WIN32
        SDL_RemoveEventWatch(WatchMouse, nullptr);
#endif
        if (g.window) SDL_DestroyWindow(g.window);
        g.window = nullptr;
#endif
#ifdef MW2_ANDROID
        if (g.nativeWindow)
        {
            android::ReleaseWindow(g.nativeWindow);
            g.nativeWindow = nullptr;
            android::WindowReleased();
        }
#endif
        g.surface = VK_NULL_HANDLE;
        g.instance = VK_NULL_HANDLE;
    }

#ifdef MW2_ANDROID
    // The Vulkan surface for the window the activity handed over.
    bool CreateSurfaceForWindow()
    {
#if defined(MW2_VULKAN_DYNAMIC)
        if (!vkCreateAndroidSurfaceKHR)
        {
            LOGE("vulkan: this driver has no VK_KHR_android_surface, so it cannot present");
            android::SetStatus("This graphics driver cannot present to the screen."
                               " Switch back to the system driver.");
            return false;
        }
#endif
        VkAndroidSurfaceCreateInfoKHR info{ VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
        info.window = g.nativeWindow;
        return Check(vkCreateAndroidSurfaceKHR(g.instance, &info, nullptr, &g.surface),
                     "vkCreateAndroidSurfaceKHR");
    }

    // The activity's surface changed: it was destroyed, or destroyed and made
    // again. Everything built on the old one goes -- in this order, and
    // before surfaceDestroyed is allowed to return -- and the new one, if
    // there is one by now, gets a swapchain.
    void AdoptNewSurface()
    {
        const uint64_t generation = android::WindowGeneration();

        if (g.device)
        {
            // Nothing of ours may still be reading or presenting the images.
            // The renderer submits to the same queue, so its lock is what
            // makes waiting for the device meaningful.
            std::lock_guard held(vk::pipeline::QueueMutex());
            vk::pipeline::Failed(vkDeviceWaitIdle(g.device), "the device before a surface change");
        }
        if (g.swapchain)
        {
            std::unique_lock life(g.swapchainLife);
            vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
            g.swapchain = VK_NULL_HANDLE;
            std::lock_guard waiting(g.waitLock);
            g.waiting.clear();
            g.lastShownNs = -1;
        }
        if (g.surface)
        {
            vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
            g.surface = VK_NULL_HANDLE;
        }
        if (g.nativeWindow)
        {
            android::ReleaseWindow(g.nativeWindow);
            g.nativeWindow = nullptr;
            // surfaceDestroyed is waiting on exactly this.
            android::WindowReleased();
        }
        LetGo();

        g.generation = generation;
        g.nativeWindow = android::AcquireWindow();
        if (!g.nativeWindow) return;          // gone for now; the loop waits
        if (!CreateSurfaceForWindow()) return;
        g.remakeSwapchain = true;
        RemakeSwapchain();
        LOGI("vulkan: presenting to the new surface (%ux%u)", g.extent.width, g.extent.height);
    }
#endif

    bool Bring()
    {
#ifdef MW2_ANDROID
        std::string error;
        crash::Step("vulkan-library", "opening the Vulkan driver");
        if (!vk::loader::Opened() && !vk::loader::Open(error))
        {
            LOGE("vulkan: %s", error.c_str());
            android::SetStatus(("No Vulkan driver could be opened: " + error).c_str());
            return false;
        }
        // The game thread runs ahead of the activity's layout; the first
        // surface can be a moment away.
        crash::Step("surface", "waiting for the screen's surface");
        while (android::Running() && !android::WaitForWindow(250)) {}
        if (!android::Running()) return false;
        g.generation = android::WindowGeneration();
        g.nativeWindow = android::AcquireWindow();
        if (!g.nativeWindow) { LOGW("vulkan: no surface to present to"); return false; }
        crash::Step("vulkan-instance", "vkCreateInstance");
        if (!CreateInstance()) {
            android::SetStatus("Failed to create Vulkan instance");
            return false;
        }
        crash::Step("vulkan-surface", "the surface, and choosing the GPU");
        if (!CreateSurfaceForWindow()) {
            android::SetStatus("Failed to create Vulkan surface for Android window");
            return false;
        }
        if (!PickDevice()) {
            android::SetStatus("No Vulkan physical device found");
            return false;
        }
        crash::Step("vulkan-device", "the device's features and extensions, and vkCreateDevice");
        if (!CreateDevice()) {
            android::SetStatus("Failed to create Vulkan logical device");
            return false;
        }
        crash::Step("swapchain", "the swapchain and its frames");
        if (!CreateSwapchain()) {
            android::SetStatus("Failed to create Vulkan swapchain");
            return false;
        }
        if (!CreateFrames()) {
            android::SetStatus("Failed to create Vulkan presentation frames");
            return false;
        }
        crash::Step("pipeline-layout", "the descriptor sets, the pipeline cache and the pipeline layout");
        vk::pipeline::Initialise(g.device, g.physical, g.queue, g.queueFamily, g.instance);
        android::SetStatus((std::string("Running on ") + vk::pipeline::DeviceName()).c_str());
        return true;
#else
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            LOGW("vulkan: SDL video would not start (%s)", SDL_GetError());
            return false;
        }
        // Resizable, and fullscreen from the start under MW2_FULLSCREEN=1 --
        // borderless at the desktop's resolution, which SDL gives a window
        // asked for fullscreen without a mode of its own. F8 toggles it.
        g.fullscreen = env::Flag("MW2_FULLSCREEN");
        g.stretch = env::Flag("MW2_STRETCH");
        // Hidden until ShowWindow. As tall as the console's frame, and as wide
        // as the title's frame is for that (render_size.h).
        const render::Size frame = render::Frame();
        g.window = SDL_CreateWindow("Modern Warfare 2", int(kHeight * frame.width / frame.height), kHeight,
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
                                    (g.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        if (!g.window) { LOGW("vulkan: no window (%s)", SDL_GetError()); return false; }
#ifdef _WIN32
        // Held from the first moment the window has the keyboard. Without
        // this the cursor walked out of the window as the player looked round,
        // and stopped turning the view at the screen's edge.
        g_mouse.used = !env::Text("MW2_MOUSE") || env::Flag("MW2_MOUSE");
        if (g_mouse.used)
        {
            SDL_AddEventWatch(WatchMouse, nullptr);
            HoldMouse();
        }
#endif
        if (!CreateInstance()) return false;
        if (!SDL_Vulkan_CreateSurface(g.window, g.instance, nullptr, &g.surface))
        {
            LOGW("vulkan: no surface (%s)", SDL_GetError());
            return false;
        }
        if (!PickDevice() || !CreateDevice() || !CreateSwapchain() || !CreateFrames())
            return false;
        // Everything else that touches the GPU -- the texture cache, the
        // renderer -- allocates against this device rather than one of its own,
        // because an image cannot be shared between two.
        vk::pipeline::Initialise(g.device, g.physical, g.queue, g.queueFamily, g.instance);
        return true;
#endif
    }

    void Worker()
    {
        crash::RegisterThread("vulkan presenter");
        if (!Bring())
        {
            LOGW("vulkan: presenting disabled, continuing headless");
            g.running = false;
            g.ready = true;
            return;
        }
        FollowTheDisplay();
        g.ready = true;
        LOGI("vulkan: presenter running");

        while (g.running.load(std::memory_order_relaxed))
        {
#ifdef MW2_ANDROID
            // The app asked for the run to end.
            if (!android::Running()) break;
            // The activity's window is on screen whenever it has a surface:
            // nothing here is hidden until asked for (vk::ShowWindow).
            g.shown = true;
            // The activity's surface came, went, or was replaced.
            if (android::WindowGeneration() != g.generation) AdoptNewSurface();
            FollowModeChange();
            // Nothing to present to, or nothing worth presenting: the frames
            // the renderer finishes are let go at once, so the guest never
            // waits on a window that is not there.
            if (!g.surface || !g.swapchain || android::Paused())
            {
                LetGo();
                std::unique_lock lock(g.lock);
                g.wake.wait_for(lock, std::chrono::milliseconds(32));
                continue;
            }
#else
            // In front, with the keyboard's focus: a window shown a moment
            // after its program started may otherwise open behind another.
            if (!g.shown && g.showAsked.load(std::memory_order_acquire))
            {
                SDL_ShowWindow(g.window);
                SDL_RaiseWindow(g.window);
                g.shown = true;
            }
            SDL_Event event;
#ifdef _WIN32
            // A menu came up or went away since the last time round.
            if (g_mouse.used) HoldMouse();
#endif
            while (SDL_PollEvent(&event))
                // Not `g.running = false`: that stops the pump, and with nothing
                // pumping, the window stops repainting and SDL stops seeing the
                // keyboard and the pad, which looks like a freeze while the guest
                // runs on headless forever. Ending the process is what closing the
                // window means. The shutdown joins this thread, so it runs on one
                // of its own and this loop keeps the window alive until it lands.
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    crash::RequestExit("the window was closed");
                // F10, F11, F9, F7 and F5 are diagnostics.
                // F10 captures the next frames that draw the world. A defect
                // that only appears in motion has to be caught by somebody
                // watching for it; no time chosen in advance will do.
                else if (diag::kOn && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                         event.key.scancode == SDL_SCANCODE_F10)
                    vk::capture::RequestNow(4);
                // F11 captures every world frame until pressed again, for
                // flicker: which frame goes wrong is not known in advance.
                // Not F12, which is RenderDoc's own key.
                else if (diag::kOn && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                         event.key.scancode == SDL_SCANCODE_F11)
                    vk::capture::ToggleStream();
                // F9 asks the title where the player stands, so a spot seen in
                // play can be given back to setviewpos. The answer is printed
                // under MW2_ENGINE_LOG=1.
                else if (diag::kOn && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                         event.key.scancode == SDL_SCANCODE_F9)
                {
                    console::RunNow("viewpos");
                    LOGI("console: F9 -- viewpos asked for");
                }
                // F5 runs the console command MW2_F5 names: `noclip`, say, once
                // a level's opening is behind.
                else if (diag::kOn && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                         event.key.scancode == SDL_SCANCODE_F5 && diag::Text("MW2_F5"))
                {
                    console::RunNow(diag::Text("MW2_F5"));
                    LOGI("console: F5 -- %s", diag::Text("MW2_F5"));
                }
                // F7: "I saw a flash just now" -- a mark in the log, and the
                // frames before it written out when MW2_FLASH_FRAMES keeps them.
                // Under MW2_STUTTERS it marks a hitch instead.
                else if (diag::kOn && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                         event.key.scancode == SDL_SCANCODE_F7)
                {
                    if (stutters::On()) stutters::Mark("F7");
                    else vk::renderer::FlashSeen("F7");
                }
                // F6 opens the friends screen, as the lobby's "Invite friends" does.
                else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                         event.key.scancode == SDL_SCANCODE_F6)
                {
                    friends::Ask();
                }
                // F8 toggles fullscreen. The size change that follows arrives as
                // an event like any other resize.
                else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                         event.key.scancode == SDL_SCANCODE_F8)
                {
                    g.fullscreen = !g.fullscreen;
                    if (!SDL_SetWindowFullscreen(g.window, g.fullscreen))
                    {
                        LOGW("vulkan: fullscreen would not toggle (%s)", SDL_GetError());
                        g.fullscreen = !g.fullscreen;
                    }
                }
#ifdef _WIN32
                // F2 gives the cursor back without leaving the window -- to
                // drag or resize it, say -- and takes it again. So does a
                // click in the window, on the button's way up, so that the
                // click is not a shot.
                else if (g_mouse.used && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                         event.key.scancode == SDL_SCANCODE_F2)
                {
                    FreeMouse(!g_mouse.freed);
                    LOGI("input: F2 -- the mouse is %s", g_mouse.freed ? "free" : "held");
                }
                else if (g_mouse.freed && event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                         event.button.button == SDL_BUTTON_LEFT)
                {
                    FreeMouse(false);
                    LOGI("input: a click -- the mouse is held");
                }
                // Coming back to the window is coming back to the game: F2
                // lasts until the window is left.
                else if (g_mouse.freed && event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
                    FreeMouse(false);
#endif
                // The window's size in pixels changed -- dragged, maximised,
                // fullscreen -- and the swapchain has to be its size.
                else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                         event.type == SDL_EVENT_WINDOW_RESIZED)
                    g.remakeSwapchain = true;
#endif  // MW2_ANDROID

            // A frame to show, or 16 ms to go round the events again. Until the
            // renderer has finished one, the window shows its own colour.
            bool queued = false;
            {
                std::unique_lock lock(g.lock);
                g.wake.wait_for(lock, std::chrono::milliseconds(16),
                                [] { return !g.frameQueue.empty() || g.holdsLeft || !g.running; });
                queued = !g.frameQueue.empty() || g.holdsLeft;
            }
            if (!g.running) break;
            // Nothing is presented to a hidden window, where a present can
            // wait for a frame the compositor never asks for.
            if (!g.shown) continue;
            if (queued) DrawOneFrame(false);
            else if (!g.presented.load(std::memory_order_relaxed)) DrawOneFrame(true);
        }
        LOGI("vulkan: presenter stopped after %llu frames, %llu swapchains",
             (unsigned long long)g.presented.load(), (unsigned long long)g.swapchainsMade);
    }
}

bool vk::Start()
{
    // Every build opens it; MW2_WINDOW=0 keeps it shut, for a headless
    // measurement run. On Android there is nothing else to be: the activity
    // is already on screen waiting for frames.
    const bool window = !env::Text("MW2_WINDOW") || env::Flag("MW2_WINDOW");
    if (!window) return false;

    g.running = true;
    g.worker = std::thread(Worker);
    while (!g.ready.load(std::memory_order_acquire)) std::this_thread::yield();
    return g.running.load();
}

void vk::ShowWindow()
{
    g.showAsked.store(true, std::memory_order_release);
    g.wake.notify_one();
}

void vk::ShowImage(void* image, uint32_t width, uint32_t height, uint64_t serial)
{
    if (!g.running.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g.lock);
    g.frameQueue.push_back({ static_cast<VkImage>(image), width, height, serial });
    g.lastQueued.store(serial, std::memory_order_relaxed);
    g.wake.notify_one();
}

void vk::WaitUntilTaken(uint64_t serial, bool (*meanwhile)())
{
    if (!serial || !g.running.load(std::memory_order_relaxed)) return;
#ifdef MW2_ANDROID
    // The activity is not in front: there is no surface to copy the frame
    // out to, and waiting a quarter of a second for one, every frame, is a
    // game running at 4 fps in the background with the phone getting hot.
    if (android::Paused())
    {
        std::lock_guard lock(g.lock);
        while (!g.frameQueue.empty() && g.frameQueue.front().serial <= serial)
            g.frameQueue.pop_front();
        if (g.held.serial <= serial) g.holdsLeft = 0;
        g.taken = std::max(g.taken, serial);
        return;
    }
#endif
    std::unique_lock lock(g.lock);
    const auto taken = [&] { return g.taken >= serial || !g.running; };
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    // What the caller owes somebody in the meantime is looked at every
    // quarter of a millisecond, without the lock, for as long as it says
    // there is more to come.
    while (meanwhile && !taken() && std::chrono::steady_clock::now() < giveUp)
    {
        if (g.takenWake.wait_for(lock, std::chrono::microseconds(250), taken)) return;
        lock.unlock();
        if (!meanwhile()) meanwhile = nullptr;
        lock.lock();
    }
    if (g.takenWake.wait_until(lock, giveUp, taken)) return;
    // A quarter of a second without the window taking a frame: it is hidden
    // or its display has stopped. The game does not wait on a window.
    while (!g.frameQueue.empty() && g.frameQueue.front().serial <= serial) g.frameQueue.pop_front();
    if (g.held.serial <= serial) g.holdsLeft = 0;
    g.taken = std::max(g.taken, serial);
    static std::atomic<uint32_t> told{ 0 };
    if (told.fetch_add(1) < 4)
        LOGW("vulkan: the window took no frame in 250 ms; frames up to %llu let go unshown",
             (unsigned long long)serial);
}

// The swapchain waits for the blank (FIFO), so the window takes a frame a
// blank and the wait for it paces the renderer. Not every swapchain does: a
// desktop driver set to present without waiting takes them as they come --
// 575 a second were shown on a 240 Hz display -- and with the activity paused
// there is no window to take any. So a frame is also never finished sooner
// than a period after the one before. The 2% under keeps this clock from
// being the one that paces where the display does: two clocks pacing the
// same frames beat against each other.
void vk::WaitForDisplay(uint64_t serial, bool (*meanwhile)())
{
    WaitUntilTaken(serial, meanwhile);
    static std::chrono::steady_clock::time_point earliest;   // the ring consumer's own
    const auto period = std::chrono::nanoseconds(int64_t(frame_rate::DisplayPeriodNs() * 0.98));
    const auto now = std::chrono::steady_clock::now();
    // Counted from when the last was due, not from when it came, so a late
    // wake-up is not added to every frame after it.
    if (now < earliest)
    {
#ifdef _WIN32
        // Windows wakes a sleeper to the millisecond at best, a quarter of a
        // 240 Hz period: the end of the wait is waited out awake.
        std::this_thread::sleep_until(earliest - std::chrono::milliseconds(2));
        while (std::chrono::steady_clock::now() < earliest) std::this_thread::yield();
#else
        std::this_thread::sleep_until(earliest);
#endif
        earliest += period;
    }
    else earliest = now + period;
}

uint64_t vk::GuestBlankPeriod()
{
    const uint64_t period = g.guestPeriodNs.load(std::memory_order_relaxed);
    // Nothing reached the screen for half a second (minimised, or no window
    // yet): there is no display clock to follow.
    if (!period || Nanoseconds() - g.guestPeriodAt.load(std::memory_order_relaxed) > 500'000'000)
        return 0;
    return period;
}

// The window thread has to stop before anything that owns an image it might be
// showing is torn down, but the device has to outlive that teardown -- so
// stopping and destroying are two steps rather than one.
void vk::StopPresenting()
{
    if (!g.worker.joinable()) return;
    g.running = false;
    { std::lock_guard lock(g.lock); g.wake.notify_all(); g.takenWake.notify_all(); }
    { std::lock_guard lock(g.waitLock); g.waitWake.notify_all(); }
    g.worker.join();
    if (g.waiter.joinable())
    {
        g.waiter.join();
        LOGI("vulkan: the display's blank measured %.1f us; frames behind the one on screen"
             " when it was shown: 0 x%llu, 1 x%llu, 2 x%llu, 3+ x%llu; the guest's blank ran"
             " at the display's for %llu, longer for %llu",
             g.periodNs / 1000.0, (unsigned long long)g.behindCount[0],
             (unsigned long long)g.behindCount[1], (unsigned long long)g.behindCount[2],
             (unsigned long long)g.behindCount[3], (unsigned long long)g.shownOnTime,
             (unsigned long long)g.stretched);
    }
}

void vk::Stop()
{
    StopPresenting();
    Teardown();
}

bool vk::Running() { return g.running.load(); }
uint64_t vk::PresentedFrames() { return g.presented.load(); }

void vk::TakeMouse(bool menu, Mouse& out)
{
    out = {};
#ifdef _WIN32
    std::lock_guard lock(g_mouse.lock);
    g_mouse.pointing = menu;
    const float dx = std::exchange(g_mouse.dx, 0.0f), dy = std::exchange(g_mouse.dy, 0.0f);
    if (!g_mouse.used || !g_mouse.focused || g_mouse.freed)
    {
        g_mouse.wheel = 0;
        return;
    }
    out.wheel = int(g_mouse.wheel);
    g_mouse.wheel -= float(out.wheel);
    if (g_mouse.held)
    {
        out.held = true;
        out.dx = dx;
        out.dy = dy;
    }
    else if (menu && g_mouse.picture[2] > g_mouse.picture[0] && g_mouse.picture[3] > g_mouse.picture[1])
    {
        out.pointing = true;
        out.x = (g_mouse.x - float(g_mouse.picture[0])) / float(g_mouse.picture[2] - g_mouse.picture[0]);
        out.y = (g_mouse.y - float(g_mouse.picture[1])) / float(g_mouse.picture[3] - g_mouse.picture[1]);
    }
#else
    (void)menu;
#endif
}

#endif  // MW2_HAVE_VULKAN && MW2_USE_SDL
