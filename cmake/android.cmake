# What the Android build of the runtime needs on top of the common one.
# Included from the top-level CMakeLists.txt when ANDROID is set, which the
# NDK's toolchain file does; the Gradle build in android/ points its
# externalNativeBuild at that same CMakeLists.txt, so this is the only place
# the phone build differs.

if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR
        "This port is arm64-v8a only. A 32-bit build cannot hold the guest's"
        " 4 GiB address space, which the recompiled code reaches with a flat"
        " 32-bit offset from one base pointer (runtime/guest_memory.cpp).")
endif()

# crash.cpp finds this library's own code in /proc/self/maps by its file name:
# lib<title>.so, or with the recompiled code in a library of its own
# (MW2_SPLIT) both that and lib<title>game.so, by what their names share.
if(MW2_SPLIT STREQUAL "runtime")
    target_compile_definitions(mw2 PRIVATE MW2_ANDROID=1 MW2_LIBRARY_FILE="lib${MW2_OUTPUT_NAME}")
    # runtime/title.h reaches the title's functions through the game library,
    # and runtime/android/game_library.cpp loads it.
    target_compile_definitions(mw2 PRIVATE MW2_SPLIT_GAME=1)
    # The global group: what the dynamic linker searches before a library's
    # own symbols, for every library loaded after this one. It is how the game
    # library's calls reach the runtime's hooks instead of the functions they
    # replace (cmake/game_library.cmake).
    target_link_options(mw2 PRIVATE -Wl,-z,global)
else()
    target_compile_definitions(mw2 PRIVATE MW2_ANDROID=1 MW2_LIBRARY_FILE="lib${MW2_OUTPUT_NAME}.so")
endif()

# 16 KB pages. Android 15 devices ship with them, and a library linked for 4 KB
# will not load at all there. Nothing in the runtime assumes a page size --
# everything asks sysconf -- so this is purely the linker's alignment.
target_link_options(mw2 PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)

# The Vulkan entry points are pointers filled in while the process runs, from
# whichever driver was opened, rather than symbols resolved against the
# platform's libvulkan.so at load time. Only the files that call Vulkan see
# the header, and they see it before anything else (runtime/gpu/vulkan/loader.h).
file(GLOB MW2_VULKAN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/*.cpp")
set_source_files_properties(${MW2_VULKAN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/loader.h")
target_compile_definitions(mw2 PRIVATE MW2_VULKAN_DYNAMIC=1)

# aaudio  the low-latency audio sink (runtime/android/audio.cpp)
# android ANativeWindow and the asset manager (runtime/android/window.cpp)
# log     __android_log_write, where the runtime's log goes (runtime/log.h)
target_link_libraries(mw2 PRIVATE aaudio android log dl)

# Symbols keep their default visibility. The crash handler resolves addresses
# through dladdr (runtime/platform.cpp), and the recompiled functions are
# named sub_XXXXXXXX: hidden, a guest stack trace would be a list of
# addresses. The symbol table costs a few hundred kilobytes and pays for
# itself the first time a player sends a log.

# ---- the player's own Vulkan driver ---------------------------------------
# libadrenotools loads a Turnip build the player imported, on a device that
# has not been rooted, by giving the driver the linker namespace it expects.
# It is not vendored here (android/fetch_deps.sh clones it); without it the
# app still runs on the system's driver and says so.
set(MW2_ADRENOTOOLS_DIR "${CMAKE_SOURCE_DIR}/third_party/libadrenotools")
if(EXISTS "${MW2_ADRENOTOOLS_DIR}/CMakeLists.txt")
    # Not EXCLUDE_FROM_ALL: its hook libraries have to be in the apk, and a
    # target excluded from the default build is not something the Android
    # plugin reliably packages. They are small.
    add_subdirectory("${MW2_ADRENOTOOLS_DIR}" adrenotools)
    target_link_libraries(mw2 PRIVATE adrenotools)
    target_compile_definitions(mw2 PRIVATE MW2_HAVE_ADRENOTOOLS=1)
    # Its two hook libraries are dlopened by the driver at run time, so they
    # have to be in the apk beside libmw2.so. Building them as part of this
    # build is what puts them there: the Gradle plugin packages every shared
    # library this CMake project produces.
    # main_hook and hook_impl are the loader itself; file_redirect_hook is
    # what ADRENOTOOLS_DRIVER_FILE_REDIRECT needs (runtime/android/driver.cpp),
    # and a driver that asks for it and cannot find it does not load.
    foreach(hook main_hook hook_impl file_redirect_hook)
        if(TARGET ${hook})
            add_dependencies(mw2 ${hook})
        endif()
    endforeach()
    message(STATUS "Custom Vulkan drivers: libadrenotools")
else()
    message(STATUS "Custom Vulkan drivers: off (run android/fetch_deps.sh to enable)")
endif()

# ---- code generation ------------------------------------------------------
# The recompiled translation units are enormous -- tens of thousands of
# functions -- and the phone's copy has to be small as well as quick.
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    # No frame pointer on the guest's own functions would make a crash report
    # useless, and the unwinder is what every backtrace here goes through.
    if(NOT MW2_SPLIT STREQUAL "runtime")
        target_compile_options(ppc_recomp PRIVATE -fno-omit-frame-pointer)
    endif()
    target_compile_options(mw2 PRIVATE -fno-omit-frame-pointer)
endif()

# The library is tens of megabytes of recompiled code and the debug info is
# several times that. It is stripped out of the apk and kept beside the build
# (android/app/build), which is what a crash report is read against.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(mw2 PRIVATE -Wl,--build-id=sha1)
endif()

# ---- both of the disc's executables in one app ------------------------------
# A game's campaign and multiplayer are two executables, recompiled into two
# trees, and one app carries both as two libraries (MW2_BOTH_TITLES); the
# run's process loads the one the player chose (android/app, Title.kt). The
# Gradle plugin drives one CMake build per variant, and nearly everything in
# this project is decided by MW2_TITLE, so the campaign's build makes the
# multiplayer's library by configuring this same project a second time, with
# the same toolchain, rather than by teaching every target above to exist
# twice. The shared runtime is compiled once for each: its code names the
# title's functions (runtime/title.h), so the two are not the same objects.
if(MW2_BOTH_TITLES)
    # The installer copies and checks both executables, and the title's own
    # way from one to the other restarts the run (runtime/install/install.cpp).
    target_compile_definitions(mw2 PRIVATE MW2_BOTH_TITLES=1)
endif()
if(MW2_BOTH_TITLES AND (MW2_TITLE STREQUAL "mw3" OR MW2_TITLE STREQUAL "sp" OR MW2_TITLE STREQUAL "cod4" OR MW2_TITLE STREQUAL "aw"))
    include(ExternalProject)
    if(MW2_IS_AW)
        set(multiplayer_title "awmp")
        set(multiplayer_library "awmp")
    elseif(MW2_IS_COD4)
        set(multiplayer_title "cod4mp")
        set(multiplayer_library "cod4mp")
    elseif(MW2_IS_MW3)
        set(multiplayer_title "mw3mp")
        set(multiplayer_library "mw3mp")
    else()
        set(multiplayer_title "mp")
        set(multiplayer_library "mw2mp")
    endif()
    # Where the finished library goes. The Gradle plugin packages only the
    # libraries that are targets of the build it started, which this one is
    # not, so the app's build names a folder it packages from besides
    # (android/app/build.gradle.kts, jniLibs).
    set(MW2_MP_LIBRARY_DIRECTORY "${CMAKE_BINARY_DIR}" CACHE PATH "Where the multiplayer's library is put for the app to package")
    file(TO_CMAKE_PATH "${MW2_MP_LIBRARY_DIRECTORY}" multiplayer_directory)
    # $CACHE{...}: what the app's build asked for (-O3). The plain variables
    # already carry the NDK toolchain's own flags, which the second configure
    # adds again for itself.
    ExternalProject_Add(multiplayer_library
        SOURCE_DIR "${CMAKE_SOURCE_DIR}"
        BINARY_DIR "${CMAKE_BINARY_DIR}/${multiplayer_library}"
        CMAKE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
            "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DCMAKE_C_FLAGS=$CACHE{CMAKE_C_FLAGS}"
            "-DCMAKE_CXX_FLAGS=$CACHE{CMAKE_CXX_FLAGS}"
            "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}"
            "-DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}"
            "-DANDROID_ABI=${ANDROID_ABI}"
            "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
            "-DANDROID_NDK=${ANDROID_NDK}"
            "-DANDROID_STL=${ANDROID_STL}"
            "-DMW2_TITLE=${multiplayer_title}"
            "-DMW2_VERSION=${MW2_VERSION}"
            -DMW2_BOTH_TITLES=ON
            "-DMW2_SPLIT=${MW2_SPLIT}"
            "-DMW3_RECOMP_ONLY=${MW3_RECOMP_ONLY}"
            "-DMW3_LINK_PROBE=${MW3_LINK_PROBE}"
            "-DMW2_DIAGNOSTICS=${MW2_DIAGNOSTICS}"
            "-DMW2_LOGGING=${MW2_LOGGING}"
            "-DMW2_LTO=${MW2_LTO}"
            "-DMW2_RELEASE_TAG=${MW2_RELEASE_TAG}"
            "-DMW3_XEX_SHA256_SP=${MW3_XEX_SHA256_SP}"
            "-DMW3_XEX_SHA256_MP=${MW3_XEX_SHA256_MP}"
            "-DMW2_XEX_SHA256_SP=${MW2_XEX_SHA256_SP}"
            "-DMW2_XEX_SHA256_MP=${MW2_XEX_SHA256_MP}"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target mw2
        # Every build asks it: only its own ninja knows whether a source changed.
        BUILD_ALWAYS ON
        USES_TERMINAL_BUILD ON
        # Only this library: the build above made the driver's hook libraries
        # a second time, and the app has this build's own.
        INSTALL_COMMAND "${CMAKE_COMMAND}" -E make_directory "${multiplayer_directory}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            <BINARY_DIR>/lib${multiplayer_library}.so "${multiplayer_directory}/lib${multiplayer_library}.so")
    # One tree at a time: each is a hundred and fifty translation units of a
    # gigabyte or more of compiler apiece, and two ninjas at once ask for
    # twice the machine. A target the app's build asks for by name (mw2) has
    # to depend on it, or it would not be built at all.
    add_dependencies(ppc_recomp multiplayer_library)
    add_dependencies(mw2 multiplayer_library)
endif()
