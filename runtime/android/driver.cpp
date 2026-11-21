// Which Vulkan the process talks to.
//
// Android's own libvulkan.so is a loader in front of the vendor driver, and
// an application cannot tell it to use another one: the ICD list lives in
// /vendor. On Adreno hardware the vendor driver is usually the reason a
// recompiled Xbox 360 title runs badly -- or not at all -- and Mesa's Turnip
// is the reason it runs. Players import it as a zip, and this file is what
// loads it.
//
// Three ways in, tried in order:
//
//   1. libadrenotools. It makes the *platform* loader open the imported
//      driver, so Android's own WSI -- surfaces, swapchains, the buffer queue
//      -- still sits above it. This is the path that works everywhere, and
//      the one the build fetches (android/fetch_deps.sh).
//   2. The driver on its own, as a plain ICD: dlopen and ask for
//      vk_icdGetInstanceProcAddr. Some Turnip builds carry their own Android
//      WSI and work like this; those that leave WSI to the platform loader
//      will fail to present, and the run falls back.
//   3. The system's libvulkan.so, which is what a build with no imported
//      driver uses.
//
// A chosen driver that does not load is never silent: the app says so on the
// driver screen, and the run carries on with the system's.
#ifdef MW2_ANDROID

#include "android.h"
#include "../env.h"
#include "../log.h"

#include <dlfcn.h>
#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/system_properties.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#if MW2_HAVE_ADRENOTOOLS
#include <adrenotools/driver.h>
#endif

namespace
{
    std::mutex g_lock;
    std::string g_directory;      // where the zip was unpacked, or empty
    std::string g_libraryName;    // "libvulkan_freedreno.so", from meta.json
    std::string g_description = "not opened yet";
    bool g_customFailed = false;
    void* g_handle = nullptr;
    void* g_getInstanceProcAddr = nullptr;

    // The Android Vulkan HAL, as hardware/hwvulkan.h declares it. Declared
    // here rather than included: the NDK does not ship those headers, and all
    // that is needed of them is the layout of two structures that have not
    // changed since Android 7.
    struct hw_module_methods_t;
    struct hw_device_t;

    struct hw_module_t
    {
        uint32_t tag;
        uint16_t module_api_version;
        uint16_t hal_api_version;
        const char* id;
        const char* name;
        const char* author;
        hw_module_methods_t* methods;
        void* dso;
        uint64_t reserved[32 - 7];
    };

    struct hw_module_methods_t
    {
        int (*open)(const hw_module_t* module, const char* id, hw_device_t** device);
    };

    struct hw_device_t
    {
        uint32_t tag;
        uint32_t version;
        hw_module_t* module;
        uint64_t reserved[12];
        int (*close)(hw_device_t* device);
    };

    struct hwvulkan_device_t
    {
        hw_device_t common;
        void* EnumerateInstanceExtensionProperties;
        void* CreateInstance;
        void* GetInstanceProcAddr;
    };

    // Every entry point a Vulkan implementation may offer the caller, in the
    // order they are worth trying. vk_icdGetInstanceProcAddr is the ICD
    // interface; vkGetInstanceProcAddr is what a loader exports.
    void* EntryPoint(void* handle, std::string& how)
    {
        if (!handle) return nullptr;

        if (void* icd = dlsym(handle, "vk_icdGetInstanceProcAddr"))
        {
            // Telling the driver which ICD interface version the "loader"
            // speaks is optional, and drivers that have it behave better for
            // having been asked.
            using Negotiate = int (*)(uint32_t*);
            if (auto negotiate = reinterpret_cast<Negotiate>(dlsym(handle, "vk_icdNegotiateLoaderICDInterfaceVersion")))
            {
                uint32_t version = 5;
                negotiate(&version);
                LOGI("driver: ICD interface version %u", version);
            }
            how = "vk_icdGetInstanceProcAddr";
            return icd;
        }
        if (void* plain = dlsym(handle, "vkGetInstanceProcAddr"))
        {
            how = "vkGetInstanceProcAddr";
            return plain;
        }
        // The Android Vulkan HAL: a hw_module_t under the symbol HMI, opened
        // to get at its GetInstanceProcAddr.
        if (void* hmi = dlsym(handle, "HMI"))
        {
            auto* module = static_cast<hw_module_t*>(hmi);
            hw_device_t* device = nullptr;
            if (module->methods && module->methods->open &&
                module->methods->open(module, "vk0", &device) == 0 && device)
            {
                auto* vulkan = reinterpret_cast<hwvulkan_device_t*>(device);
                if (vulkan->GetInstanceProcAddr)
                {
                    how = "the Android Vulkan HAL";
                    return vulkan->GetInstanceProcAddr;
                }
            }
        }
        return nullptr;
    }

    std::string JoinPath(const std::string& directory, const std::string& name)
    {
        if (directory.empty()) return name;
        if (directory.back() == '/') return directory + name;
        return directory + "/" + name;
    }

    // With a trailing slash, which is how libadrenotools wants its paths.
    std::string AsDirectory(const std::string& path)
    {
        if (path.empty() || path.back() == '/') return path;
        return path + "/";
    }

    // Every early refusal inside adrenotools_open_libvulkan is a bare
    // "return nullptr" with no dlerror set, which is why a failure there
    // reads as "no reason given". Two of them are about directories that
    // have to exist before the call: the one it writes the patched driver
    // into, and the one it redirects the driver's own file writes to. So
    // they are made here, and everything it is about to be told is checked
    // and said out loud first.
    bool MakeDirectory(const std::string& path)
    {
        if (path.empty()) return false;
        std::string without = path;
        while (without.size() > 1 && without.back() == '/') without.pop_back();
        if (::mkdir(without.c_str(), 0700) == 0) return true;
        return errno == EEXIST;
    }

    bool Exists(const std::string& path)
    {
        struct stat info{};
        return ::stat(path.c_str(), &info) == 0;
    }

    // Turnip compresses the images it can (UBWC), and on an Adreno 6xx that
    // hard-resets the whole device the moment a level's world is first drawn:
    // no fault in the kernel log, the power controller reporting a shutdown
    // the SoC asked for. Seen on the Adreno 650 (Retroid Pocket Flip 2,
    // kernel 4.19) with Mesa 25.1 and 27.0 alike, at every one of a dozen
    // level starts of Modern Warfare 3's Survival, and at none with
    // TU_DEBUG=noubwc, which costs no measurable frame time there. Modern
    // Warfare 2's Team Player started six times with the compression on and
    // did not reset, and was no faster for it (by image class or whole), so
    // it is off for both. The driver reads TU_DEBUG once, as its
    // first instance is made, so it is set here, before the driver is opened.
    // MW2_ADRENO6_UBWC=1 leaves the compression on, to study the reset.
    void KeepTurnipOffUbwcOnAdreno6()
    {
        if (!android::perf::Adreno6xx()) return;
        const char* model = android::perf::AdrenoModel();
        if (env::Flag("MW2_ADRENO6_UBWC"))
        {
            LOGW("driver: %s with Turnip's image compression left on (MW2_ADRENO6_UBWC):"
                 " expect the device to reset when a level starts", model);
            return;
        }
        // Whatever MW2_DRIVER_ENV asked of the driver stays.
        const char* asked = std::getenv("TU_DEBUG");
        std::string flags = asked ? asked : "";
        if (flags.find("noubwc") == std::string::npos)
        {
            if (!flags.empty()) flags += ',';
            flags += "noubwc";
            setenv("TU_DEBUG", flags.c_str(), 1);
        }
        LOGI("driver: %s: Turnip draws without image compression (TU_DEBUG=%s); with it an"
             " Adreno 6xx resets the device when a level starts", model, flags.c_str());
    }

    std::string Property(const char* name)
    {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get(name, value);
        return value;
    }

    // Whether the chip is a Qualcomm one: the kernel's Adreno driver names
    // it, or the build says whose graphics libraries or whose chip it has.
    // Any one of them, because a vendor's kernel may hide the first from an
    // app and an old build has no ro.soc.manufacturer.
    bool AdrenoHardware()
    {
        if (android::perf::AdrenoModel()[0]) return true;
        for (const char* name : { "ro.hardware.vulkan", "ro.hardware.egl" })
            if (Property(name).find("adreno") != std::string::npos) return true;
        const std::string maker = Property("ro.soc.manufacturer");
        return maker == "QTI" || maker.find("Qualcomm") != std::string::npos;
    }

    // Turnip is Mesa's driver for Adreno and drives nothing else. Chosen on
    // a Mali, a PowerVR or an Xclipse -- a settings backup restored on another
    // phone, a zip imported out of curiosity -- it would at best load and find
    // no device, after libadrenotools had patched the linker for it.
    bool TurnipWithoutAdreno(const std::string& library)
    {
        std::string lower = library;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        const bool turnip = lower.find("freedreno") != std::string::npos ||
                            lower.find("turnip") != std::string::npos;
        return turnip && !AdrenoHardware();
    }
}

void android::driver::Select(const char* directory, const char* libraryName)
{
    std::lock_guard lock(g_lock);
    g_directory = directory ? directory : "";
    g_libraryName = libraryName ? libraryName : "";
    if (g_directory.empty() || g_libraryName.empty())
        LOGI("driver: the system's Vulkan");
    else
        LOGI("driver: %s from %s", g_libraryName.c_str(), g_directory.c_str());
}

const std::string& android::driver::Description() { return g_description; }
bool android::driver::CustomDriverFailed() { return g_customFailed; }

void* android::driver::Open(std::string& error)
{
    std::lock_guard lock(g_lock);
    if (g_getInstanceProcAddr) return g_getInstanceProcAddr;

    const Paths& paths = GetPaths();
    std::string how;

    const bool refused = !g_directory.empty() && TurnipWithoutAdreno(g_libraryName);
    if (refused)
        LOGW("driver: %s is Turnip, Mesa's driver for Adreno, and this chip is not one"
             " (ro.hardware.vulkan %s, ro.soc.manufacturer %s): the system's Vulkan is used",
             g_libraryName.c_str(), Property("ro.hardware.vulkan").c_str(),
             Property("ro.soc.manufacturer").c_str());

    if (!refused && !g_directory.empty() && !g_libraryName.empty())
    {
        // An imported driver on this hardware is Turnip, however it is loaded.
        KeepTurnipOffUbwcOnAdreno6();
#if MW2_HAVE_ADRENOTOOLS
        // The imported driver, loaded so that Android's WSI stays above it.
        // The temporary folder is where the loader writes the patched copy of
        // the driver, and the redirect folder is where a driver that wants to
        // write beside itself ends up (Turnip's debug files, a shader dump).
        const std::string temporary = AsDirectory(paths.cache) + "driver_tmp/";
        const std::string redirect = AsDirectory(paths.files) + "driver_files/";
        if (!MakeDirectory(temporary))
            LOGW("driver: could not make %s", temporary.c_str());
        if (!MakeDirectory(redirect))
            LOGW("driver: could not make %s", redirect.c_str());
        // Turnip's disassembler opens /dev/null to write a first pass into,
        // and the redirect sends that here too: without the folder the file
        // is not opened and the driver aborts the process writing to it
        // ("FORTIFY: fputc: null FILE*"), which is every pipeline built
        // under IR3_SHADER_DEBUG=disasm or MW2_SHADER_IR.
        if (!MakeDirectory(redirect + "dev/"))
            LOGW("driver: could not make %sdev/", redirect.c_str());

        // The hook libraries are dlopened by name out of the apk's own
        // library directory. If the build did not package them there is
        // nothing adrenotools can do, and it will not say so itself.
        const std::string hooks = AsDirectory(paths.nativeLib);
        for (const char* hook : { "libmain_hook.so", "libhook_impl.so" })
        {
            if (!Exists(hooks + hook))
                LOGW("driver: %s%s is missing -- it is not in this apk, so the imported"
                     " driver cannot be loaded", hooks.c_str(), hook);
        }
        if (!Exists(AsDirectory(g_directory) + g_libraryName))
            LOGW("driver: %s%s is not there", AsDirectory(g_directory).c_str(),
                 g_libraryName.c_str());

        g_handle = adrenotools_open_libvulkan(
            RTLD_NOW | RTLD_LOCAL,
            ADRENOTOOLS_DRIVER_CUSTOM | ADRENOTOOLS_DRIVER_FILE_REDIRECT,
            temporary.c_str(), paths.nativeLib.c_str(),
            AsDirectory(g_directory).c_str(), g_libraryName.c_str(),
            redirect.c_str(), nullptr);
        if (g_handle)
        {
            g_getInstanceProcAddr = EntryPoint(g_handle, how);
            if (g_getInstanceProcAddr)
            {
                g_description = g_libraryName + " (adrenotools, " + how + ")";
                LOGI("driver: %s", g_description.c_str());
                return g_getInstanceProcAddr;
            }
            dlclose(g_handle);
            g_handle = nullptr;
        }
        // Its refusals are silent, so the reason is worth narrowing down.
        const char* err = dlerror();
        LOGW("driver: adrenotools could not load %s (%s); driver dir %s, hooks %s,"
             " redirect %s", g_libraryName.c_str(),
             err ? err : "it refused before opening anything",
             Exists(AsDirectory(g_directory) + g_libraryName) ? "ok" : "MISSING",
             Exists(hooks + "libmain_hook.so") ? "ok" : "MISSING",
             Exists(redirect) ? "ok" : "MISSING");
#endif
        // Straight dlopen. A driver that carries its own Android WSI works;
        // one that expects the platform loader above it will not present, and
        // the presenter's own checks send the run back to the system driver.
        const std::string path = JoinPath(g_directory, g_libraryName);
        g_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (g_handle)
        {
            g_getInstanceProcAddr = EntryPoint(g_handle, how);
            if (g_getInstanceProcAddr)
            {
                g_description = g_libraryName + " (" + how + ")";
                LOGI("driver: %s, loaded directly", g_description.c_str());
                return g_getInstanceProcAddr;
            }
            LOGW("driver: %s has no Vulkan entry point", path.c_str());
            dlclose(g_handle);
            g_handle = nullptr;
        }
        else
        {
            const char* why = dlerror();
            LOGW("driver: %s would not load (%s)", path.c_str(), why ? why : "?");
#if !MW2_HAVE_ADRENOTOOLS
            // Worth spelling out, because the message above names a missing
            // system library and reads like the driver's fault. It is not:
            // a Turnip build links against libcutils and the rest of
            // /system/lib64, which an app's own linker namespace cannot see.
            // Giving it a namespace that can is the whole job of
            // libadrenotools, and this build was made without it.
            LOGW("driver: this build has no libadrenotools, so an imported driver can only be"
                 " loaded if it needs nothing from /system -- which a Turnip build does."
                 " Run android/fetch_deps.sh and build again.");
#endif
        }
        g_customFailed = true;
    }

    // The system's loader.
    for (const char* name : { "libvulkan.so", "libvulkan.so.1" })
    {
        g_handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (!g_handle) continue;
        g_getInstanceProcAddr = EntryPoint(g_handle, how);
        if (g_getInstanceProcAddr)
        {
            g_description = std::string("the system's ") + name;
            if (refused) g_description += " (the imported Turnip is for Adreno chips and was not loaded)";
            LOGI("driver: %s", g_description.c_str());
            return g_getInstanceProcAddr;
        }
        dlclose(g_handle);
        g_handle = nullptr;
    }

    const char* why = dlerror();
    error = std::string("no Vulkan on this device (") + (why ? why : "libvulkan.so is missing") + ")";
    g_description = "none";
    LOGE("driver: %s", error.c_str());
    return nullptr;
}

#endif  // MW2_ANDROID
