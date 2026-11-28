cmake_minimum_required(VERSION 3.20)
project(mw2recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(MW2_TESTS_ONLY "Configure only asset-free runtime unit tests" OFF)
# Android builds the runtime as the app's library (cmake/android.cmake); the
# host checks below are not cross-compiled for the phone.
if(ANDROID)
    set(BUILD_TESTING OFF CACHE BOOL "Build the tests (not on Android)" FORCE)
endif()
include(CTest)
# For the checks that assert rather than report, so `ctest` is a yes or a no.
# Everything registered runs without game files, a disc or a GPU.
enable_testing()
if(BUILD_TESTING)
    add_executable(xex-header-test tests/xex_header_test.cpp runtime/install/xex.cpp runtime/install/crypto.cpp)
    target_include_directories(xex-header-test PRIVATE "${CMAKE_SOURCE_DIR}")
    add_test(NAME xex-header-test COMMAND xex-header-test)
    add_executable(guest-format-test tests/guest_format_test.cpp)
    target_include_directories(guest-format-test PRIVATE "${CMAKE_SOURCE_DIR}")
    add_test(NAME guest-format-test COMMAND guest-format-test)
    add_executable(guest-layout-test tests/guest_layout_test.cpp)
    target_include_directories(guest-layout-test PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(guest-layout-test PRIVATE MW3_TITLE=1)
    add_test(NAME guest-layout-test COMMAND guest-layout-test)
    add_executable(dpc-queue-test tests/dpc_queue_test.cpp)
    target_include_directories(dpc-queue-test PRIVATE "${CMAKE_SOURCE_DIR}")
    find_package(Threads)
    if(Threads_FOUND)
        target_link_libraries(dpc-queue-test PRIVATE Threads::Threads)
    endif()
    add_test(NAME dpc-queue-test COMMAND dpc-queue-test)
    # Runtime tests: the runtime's own kernel and GPU sources, compiled unchanged against
    # a fake guest memory (tests/fake_guest.h) and a stub of the generated
    # header (tests/stubs). Synthetic data only; no game files are involved.
    function(mw3_runtime_test name)
        add_executable(${name} tests/${name}.cpp ${ARGN})
        target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/tests/stubs")
        if(Threads_FOUND)
            target_link_libraries(${name} PRIVATE Threads::Threads)
        endif()
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 60)
    endfunction()
    mw3_runtime_test(kernel_heap_test)
    # The kernel's hash functions against digests worked out elsewhere.
    mw3_runtime_test(sha_test runtime/kernel/sha.cpp)
    mw3_runtime_test(kernel_memory_test runtime/kernel/memory.cpp)
    mw3_runtime_test(kernel_threads_test runtime/kernel/threads.cpp runtime/kernel/objects.cpp)
    mw3_runtime_test(kernel_objects_test runtime/kernel/objects.cpp)
    mw3_runtime_test(kernel_files_test runtime/kernel/files.cpp runtime/kernel/volume.cpp runtime/kernel/objects.cpp
                     runtime/kernel/durable.cpp runtime/install/crypto.cpp launcher/disc.cpp)
    # Which of Advanced Warfare's executables is given which of a player's
    # packages: the rules alone, on packages the test names.
    mw3_runtime_test(content_route_test)
    mw3_runtime_test(kernel_dpc_test runtime/kernel/dpc.cpp runtime/kernel/objects.cpp)
    mw3_runtime_test(kernel_xam_test runtime/kernel/xam.cpp runtime/kernel/objects.cpp)
    mw3_runtime_test(kernel_sync_test runtime/kernel/sync.cpp runtime/kernel/objects.cpp)
    mw3_runtime_test(kernel_input_test runtime/kernel/input.cpp runtime/console_queue.cpp)
    mw3_runtime_test(kernel_profile_test runtime/kernel/profile.cpp runtime/kernel/durable.cpp)
    add_test(NAME kernel-input-scripted COMMAND kernel_input_test scripted)
    set_tests_properties(kernel-input-scripted PROPERTIES TIMEOUT 60)
    # The PM4 command processor, fed streams built by the test, with a recording fake
    # standing in for the Vulkan renderer.
    mw3_runtime_test(gpu_command_processor_test runtime/gpu/command_processor.cpp)
    # The camouflage pack reader, on packs the test writes.
    mw3_runtime_test(camo_packs_test runtime/camo_packs.cpp)
    # Advanced Warfare's and Black Ops III's stepped-over instructions against
    # their definitions.
    # They are written against the recompiler's register types, so the check
    # is there only where the recompiler's checkout is (setup clones it; a CI
    # runner of the asset-free tests has none).
    if(EXISTS "${CMAKE_SOURCE_DIR}/XenonRecomp/XenonUtils/ppc_context.h")
        add_executable(ppc_extra_instructions_test tests/ppc_extra_instructions_test.cpp
                       runtime/recomp/ppc_extra_instructions.cpp)
        # The recompiler's header wants the title's generated extents first
        # (ppc_config.h, made from the disc); neither the hooks nor the check
        # reads them, so it is told they are there.
        target_compile_definitions(ppc_extra_instructions_test PRIVATE AW_TITLE=1 BO3_TITLE=1 PPC_CONFIG_H_INCLUDED=1)
        target_include_directories(ppc_extra_instructions_test PRIVATE
            "${CMAKE_SOURCE_DIR}/XenonRecomp/XenonUtils" "${CMAKE_SOURCE_DIR}/XenonRecomp/thirdparty/simde")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            # The level the game itself is compiled for (MW2_X86_LEVEL below).
            target_compile_options(ppc_extra_instructions_test PRIVATE -march=x86-64-v2)
        endif()
        add_test(NAME ppc_extra_instructions_test COMMAND ppc_extra_instructions_test)
    endif()
endif()
# Stop before title selection, generated PPC sources and dependency downloads.
if(MW2_TESTS_ONLY)
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR "MW2_TESTS_ONLY requires BUILD_TESTING=ON")
    endif()
    return()
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Android brings its own window, input and audio (runtime/android/), so SDL is
# off there whatever the cache says -- and a default the Gradle build does not
# have to repeat.
if(ANDROID)
    set(MW2_USE_SDL OFF CACHE BOOL "Use SDL3 (not on Android)" FORCE)
endif()
option(MW2_USE_SDL "Use SDL3 for gamepad input (and later window/audio)" ON)
option(MW2_TRACE_INDIRECT "Report indirect calls with no recompiled target instead of jumping to null" ON)
option(MW2_LOGGING "Emit the runtime's log" ON)
option(MW2_PORTABLE "Link the C++ runtime statically, for a binary that runs on other machines" OFF)
option(MW2_DIAGNOSTICS "Build the traces, dumps, statistics, reports and test harness (runtime/diagnostics.h)" ON)
option(MW3_RECOMP_ONLY "Build only the MW3 generated PPC library; runtime hooks are not ready yet" OFF)
option(MW3_LINK_PROBE "Link a diagnostic MW3 desktop executable after the compile-only checks" OFF)

# The x86-64 level the game is compiled for. The 360's vector unit reads memory
# big-endian, so nearly every vector load, store and permute of the recompiled
# code is a byte shuffle: one pshufb from SSSE3 on, sixteen single-byte moves
# below it, where SIMDe has nothing better. x86-64-v2 adds SSSE3, SSE4.1, SSE4.2
# and POPCNT: Intel Core processors since 2008, AMD since 2011. The game says
# so on an older one (runtime/cpu_level.cpp) rather than stopping on the first
# instruction it lacks.
set(MW2_X86_LEVEL "x86-64-v2" CACHE STRING "The -march the game is compiled for")
# An Android build is arm64 (cmake/android.cmake), where the level means nothing.
if(ANDROID)
    set(MW2_ARCH_OPTIONS "")
else()
    set(MW2_ARCH_OPTIONS "-march=${MW2_X86_LEVEL}")
endif()

# ---- which executable ------------------------------------------------------
# The disc carries two: default.xex (the campaign and special ops) and
# default_mp.xex (the multiplayer). Each is recompiled into its own tree by
# build.sh (TITLE=sp|mp), and the runtime names the title's functions through
# runtime/title.h, which this definition selects.
set(MW2_TITLE "sp" CACHE STRING "Which executable to build: sp, mp, mw3, mw3mp, cod4, cod4mp, aw, awmp, awmp17, bo3, or bo3zm")
# Modern Warfare 3's two executables are titles of their own: mw3 is its
# campaign (default.xex) and mw3mp its multiplayer (default_mp.xex,
# docs/MW3-MP-BRINGUP.md). MW2_IS_MW3 is set for both.
# Call of Duty 4's are cod4 and cod4mp (docs/COD4-BRINGUP.md). They are built
# the way Modern Warfare 3's are -- the disc's executable, a hook only where
# runtime/title.h has an entry for it -- so MW2_IS_MW3 and MW3_TITLE are set
# for them too, and MW2_IS_COD4 and COD4_TITLE say what is their own.
# Advanced Warfare's are aw and awmp (docs/AW-BRINGUP.md), built the same way,
# with MW2_IS_AW and AW_TITLE for what is their own. awmp17 is its multiplayer
# as title update 17 makes it, which the downloadable content needs
# (AW_TITLE_TU17 beside AW_TITLE_MP; docs/dlc.md).
# Black Ops III's are bo3 (default.xex, which on this console is the
# multiplayer) and bo3zm (default_zm.xex, the zombies; docs/BO3-BRINGUP.md),
# as title update 5 patches them, built the same way, with MW2_IS_BO3 and
# BO3_TITLE (BO3_TITLE_ZM).
set(MW2_IS_MW3 OFF)
set(MW2_IS_COD4 OFF)
set(MW2_IS_AW OFF)
set(MW2_IS_BO3 OFF)
if(MW2_TITLE STREQUAL "sp")
    set(MW2_PPC_DIR "${CMAKE_SOURCE_DIR}/ppc")
elseif(MW2_TITLE STREQUAL "mp")
    set(MW2_PPC_DIR "${CMAKE_SOURCE_DIR}/ppc_mp")
    add_compile_definitions(MW2_TITLE_MP=1)
elseif(MW2_TITLE STREQUAL "mw3" OR MW2_TITLE STREQUAL "mw3mp")
    set(MW2_IS_MW3 ON)
    if(NOT MW3_RECOMP_ONLY)
        message(FATAL_ERROR "MW3 runtime integration is incomplete; configure with -DMW3_RECOMP_ONLY=ON to validate generated PPC code")
    endif()
    add_compile_definitions(MW3_TITLE=1)
    if(MW2_TITLE STREQUAL "mw3mp")
        # Not MW2_TITLE_MP: that selects MW2's multiplayer fixes and network.
        set(MW2_PPC_DIR "${CMAKE_SOURCE_DIR}/ppc_mw3_mp")
        add_compile_definitions(MW3_TITLE_MP=1)
    else()
        set(MW2_PPC_DIR "${CMAKE_SOURCE_DIR}/ppc_mw3")
    endif()
elseif(MW2_TITLE STREQUAL "cod4" OR MW2_TITLE STREQUAL "cod4mp")
    set(MW2_IS_MW3 ON)
    set(MW2_IS_COD4 ON)
    if(NOT MW3_RECOMP_ONLY)
        message(FATAL_ERROR "Call of Duty 4 configures as Modern Warfare 3 does: -DMW3_RECOMP_ONLY=ON -DMW3_LINK_PROBE=ON")
    endif()
    add_compile_definitions(MW3_TITLE=1 COD4_TITLE=1)
    if(MW2_TITLE STREQUAL "cod4mp")
        set(MW2_PPC_DIR "${CMAKE_SOURCE_DIR}/ppc_cod4_mp")
        add_compile_definitions(COD4_TITLE_MP=1)
    else()
        set(MW2_PPC_DIR "${CMAKE_SOURCE_DIR}/ppc_cod4")
    endif()
elseif(MW2_TITLE STREQUAL "aw" OR MW2_TITLE STREQUAL "awmp" OR MW2_TITLE STREQUAL "awmp17")
    set(MW2_IS_MW3 ON)
    set(MW2_IS_AW ON)
    if(NOT MW3_RECOMP_ONLY)
        message(FATAL_ERROR "Advanced Warfare configures as Modern Warfare 3 does: -DMW3_RECOMP_ONLY=ON -DMW3_LINK_PROBE=ON")
    endif()
    add_compile_definitions(MW3_TITLE=1 AW_TITLE=1)
    if(MW2_TITLE STREQUAL "awmp17")
        set(MW2_PPC_DIR "${CMAKE_SOURCE_DIR}/ppc_aw_mp_tu17")
        add_compile_definitions(AW_TITLE_MP=1 AW_TITLE_TU17=1)
    elseif(MW2_TITLE STREQUAL "awmp")
        set(MW2_PPC_DIR "${CMAKE_SOURCE_DIR}/ppc_aw_mp")
        add_compile_definitions(AW_TITLE_MP=1)
    else()
        set(MW2_PPC_DIR "${CMAKE_SOURCE_DIR}/ppc_aw")
    endif()
elseif(MW2_TITLE STREQUAL "bo3" OR MW2_TITLE STREQUAL "bo3zm")
    set(MW2_IS_MW3 ON)
    set(MW2_IS_BO3 ON)
    if(NOT MW3_RECOMP_ONLY)
        message(FATAL_ERROR "Black Ops III configures as Modern Warfare 3 does: -DMW3_RECOMP_ONLY=ON -DMW3_LINK_PROBE=ON")
    endif()
    add_compile_definitions(MW3_TITLE=1 BO3_TITLE=1)
    if(MW2_TITLE STREQUAL "bo3zm")
        set(MW2_PPC_DIR "${CMAKE_SOURCE_DIR}/ppc_bo3_zm")
        add_compile_definitions(BO3_TITLE_ZM=1)
    else()
        set(MW2_PPC_DIR "${CMAKE_SOURCE_DIR}/ppc_bo3")
    endif()
else()
    message(FATAL_ERROR "MW2_TITLE must be sp, mp, mw3, mw3mp, cod4, cod4mp, aw, awmp, awmp17, bo3, or bo3zm, not '${MW2_TITLE}'")
endif()
# And which version of it: the one title update 6 makes of the disc's (tu6),
# or the disc's own (tu0), recompiled into ppc_tu0/ and ppc_mp_tu0/.
# On Android the TU dimension of android/app/build.gradle.kts picks this with
# -DMW2_VERSION=tu6; tu0 is the default there for MW2 (MW3 has only its disc
# executable, built as tu6 below, on every platform). TU6 trees are generated on the CI
# runner (build.sh VERSION=tu6 regenerates ppc//ppc_mp/, or ppc_tu6//ppc_mp_tu6/
# when those names are used); a tu6 configure without a TU6 tree fails below
# at the empty-tree check rather than miscompiling the disc's tree as TU6.
if(ANDROID AND NOT MW2_IS_MW3)
    set(MW2_VERSION "tu0" CACHE STRING "Which version of the executable: tu6 (title update 6) or tu0 (the disc's)")
    if(MW2_VERSION STREQUAL "tu0")
        add_compile_definitions(MW2_VERSION_TU0=1)
        if(EXISTS "${MW2_PPC_DIR}_tu0")
            set(MW2_PPC_DIR "${MW2_PPC_DIR}_tu0")
        endif()
    elseif(MW2_VERSION STREQUAL "tu6")
        if(EXISTS "${MW2_PPC_DIR}_tu6")
            set(MW2_PPC_DIR "${MW2_PPC_DIR}_tu6")
        endif()
    else()
        message(FATAL_ERROR "MW2_VERSION must be tu6 or tu0, not '${MW2_VERSION}'")
    endif()
else()
    set(MW2_VERSION "tu6" CACHE STRING "Which version of the executable: tu6 (title update 6) or tu0 (the disc's)")
    if(MW2_VERSION STREQUAL "tu0")
        if(EXISTS "${MW2_PPC_DIR}_tu0")
            set(MW2_PPC_DIR "${MW2_PPC_DIR}_tu0")
        endif()
        add_compile_definitions(MW2_VERSION_TU0=1)
    elseif(NOT MW2_VERSION STREQUAL "tu6")
        message(FATAL_ERROR "MW2_VERSION must be tu6 or tu0, not '${MW2_VERSION}'")
    endif()
endif()
if(MW2_IS_MW3 AND NOT MW2_VERSION STREQUAL "tu6")
    message(FATAL_ERROR "MW3 currently uses its disc executable; leave MW2_VERSION at its default")
endif()

# ---- the recompiled code as a library of its own ----------------------------
# Android: the app's library without the recompiled code (runtime), which can
# be published and updated, and the recompiled code's (game), which each
# player builds from their own disc. OFF is one library or executable with
# both, as everywhere else. cmake/game_abi.cmake says what the two halves
# agree on, and for the runtime stands in for the recompiled tree.
set(MW2_SPLIT "OFF" CACHE STRING "Android: OFF, runtime or game (docs/game-library.md)")
# One app with the campaign's library and the multiplayer's (cmake/android.cmake).
option(MW2_BOTH_TITLES "Android: the app carries the campaign and the multiplayer" OFF)
# What the title's library, or executable, is called. Modern Warfare 3's is
# named for its title (mw3, mw3mp). Modern Warfare 2's is mw2 whatever the
# title, as its two apps of one title each always had it, except where both
# titles' libraries meet in one app or one game package: there the
# multiplayer's is mw2mp.
if(MW2_IS_MW3)
    set(MW2_LIBRARY_NAME "${MW2_TITLE}")
elseif(MW2_TITLE STREQUAL "mp" AND (MW2_BOTH_TITLES OR NOT MW2_SPLIT STREQUAL "OFF"))
    set(MW2_LIBRARY_NAME "mw2mp")
else()
    set(MW2_LIBRARY_NAME "mw2")
endif()
if(NOT MW2_SPLIT STREQUAL "OFF")
    include(cmake/game_abi.cmake)
endif()

# ---- how the code is optimised ---------------------------------------------
# Every target below, the libraries fetched included, is compiled for ThinLTO,
# optimised across files at the link, and with the profile in pgo/: the counts
# of a recorded play (tools/pgo_record.sh) that tell the compiler which
# branches are taken and which calls are hot. Together they take ~13% off the
# CPU time of the thread that limits the frame rate (docs/building.md).
# MW2_PGO_TRAINING builds the counting executable the profile is recorded with.
option(MW2_LTO "Optimise across files at the link (ThinLTO)" ON)
option(MW2_PGO "Optimise with the recorded profile in pgo/, when there is one" ON)
option(MW2_PGO_TRAINING "Build the executable that records a profile (tools/pgo_record.sh)" OFF)
# Where the linker keeps what ThinLTO made of each module, for the next link to
# reuse what did not change. A result taken from it is the one the linker would
# have made, so a folder kept between builds (the release workflow's) only
# makes the link faster.
set(MW2_LTO_CACHE "${CMAKE_BINARY_DIR}/lto-cache" CACHE PATH "The ThinLTO cache folder")
if(MW2_PGO_TRAINING)
    add_compile_options(-fprofile-instr-generate)
    add_link_options(-fprofile-instr-generate)
else()
    if(MW2_LTO)
        add_compile_options(-flto=thin)
        add_link_options(-flto=thin "-Wl,--thinlto-cache-dir=${MW2_LTO_CACHE}")
        if(NOT WIN32)
            # The system linker reads no LLVM bitcode; llvm-mingw's already is lld.
            add_link_options(-fuse-ld=lld)
        endif()
        # Static libraries of bitcode need LLVM's archiver to be indexed; CMake
        # takes it with clang.
        if(NOT CMAKE_AR MATCHES "llvm-ar")
            message(FATAL_ERROR "ThinLTO needs llvm-ar, not ${CMAKE_AR}: build with clang (build.sh), or -DMW2_LTO=OFF")
        endif()
    endif()
    # The profile of this executable, unpacked into the build folder under a
    # name its hash makes, so that a new one rebuilds everything with it. The
    # disc's version (tu0) is other code, and has none.
    set(MW2_PGO_ARCHIVE "${CMAKE_SOURCE_DIR}/pgo/${MW2_TITLE}.tar.xz")
    # The recorded profiles are of the x86-64 desktop build, not the phone's.
    if(MW2_PGO AND NOT ANDROID AND MW2_VERSION STREQUAL "tu6" AND EXISTS "${MW2_PGO_ARCHIVE}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MW2_PGO_ARCHIVE}")
        file(SHA256 "${MW2_PGO_ARCHIVE}" hash)
        string(SUBSTRING "${hash}" 0 16 hash)
        set(profile_dir "${CMAKE_BINARY_DIR}/pgo-${hash}")
        if(NOT EXISTS "${profile_dir}/${MW2_TITLE}.profdata")
            file(ARCHIVE_EXTRACT INPUT "${MW2_PGO_ARCHIVE}" DESTINATION "${profile_dir}")
        endif()
        # A function changed since the recording is compiled without its counts.
        add_compile_options("-fprofile-instr-use=${profile_dir}/${MW2_TITLE}.profdata"
                            -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
        message(STATUS "Profile: ${MW2_PGO_ARCHIVE}")
    else()
        message(STATUS "Profile: none")
    endif()
endif()

# ---- recompiled guest code -------------------------------------------------
# Guard: a TU6 tree's entry point differs from the disc's (SP 0x82370940 vs
# 0x82370938, MP 0x823A90C8 vs 0x823A7FE0), and the mapping names it. A build
# whose tree is the other version's fails here instead of miscompiling it.
# The app's library alone (MW2_SPLIT=runtime) has no tree to mistake.
if(MW2_SPLIT STREQUAL "runtime")
elseif((MW2_VERSION STREQUAL "tu6" AND MW2_TITLE STREQUAL "sp") OR
   (MW2_VERSION STREQUAL "tu0" AND MW2_TITLE STREQUAL "sp" AND ANDROID))
    file(READ "${MW2_PPC_DIR}/ppc_func_mapping.cpp" _mw2_mapping)
    string(TOLOWER "${_mw2_mapping}" _mw2_mapping)
    if(MW2_VERSION STREQUAL "tu6" AND NOT _mw2_mapping MATCHES "0x82370940")
        message(FATAL_ERROR "${MW2_PPC_DIR} is not a TU6 campaign tree -- run TITLE=sp VERSION=tu6 ./build.sh first")
    elseif(MW2_VERSION STREQUAL "tu0" AND NOT _mw2_mapping MATCHES "0x82370938")
        message(FATAL_ERROR "${MW2_PPC_DIR} is not a TU0 campaign tree")
    endif()
endif()
if(MW2_SPLIT STREQUAL "runtime")
elseif((MW2_VERSION STREQUAL "tu6" AND MW2_TITLE STREQUAL "mp") OR
   (MW2_VERSION STREQUAL "tu0" AND MW2_TITLE STREQUAL "mp" AND ANDROID))
    file(READ "${MW2_PPC_DIR}/ppc_func_mapping.cpp" _mw2_mapping)
    string(TOLOWER "${_mw2_mapping}" _mw2_mapping)
    if(MW2_VERSION STREQUAL "tu6" AND NOT _mw2_mapping MATCHES "0x823a90c8")
        message(FATAL_ERROR "${MW2_PPC_DIR} is not a TU6 multiplayer tree -- run TITLE=mp VERSION=tu6 ./build.sh first")
    elseif(MW2_VERSION STREQUAL "tu0" AND NOT _mw2_mapping MATCHES "0x823a7fe0")
        message(FATAL_ERROR "${MW2_PPC_DIR} is not a TU0 multiplayer tree")
    endif()
endif()
if(MW2_SPLIT STREQUAL "runtime")
    # Headers only (cmake/game_abi.cmake): the code is the game library's.
    add_library(ppc_recomp INTERFACE)
    set(ppc_scope INTERFACE)
else()
    file(GLOB PPC_SOURCES CONFIGURE_DEPENDS "${MW2_PPC_DIR}/ppc_recomp.*.cpp")
    list(APPEND PPC_SOURCES "${MW2_PPC_DIR}/ppc_func_mapping.cpp")
    if(NOT PPC_SOURCES)
        message(FATAL_ERROR "${MW2_PPC_DIR} is empty -- run TITLE=${MW2_TITLE} VERSION=${MW2_VERSION} ./build.sh first to generate it")
    endif()
    if(MW2_SPLIT STREQUAL "game")
        add_library(ppc_recomp SHARED ${PPC_SOURCES} "${MW2_GAME_ABI_SOURCE}")
    else()
        add_library(ppc_recomp STATIC ${PPC_SOURCES})
    endif()
    set(ppc_scope PUBLIC)
endif()
set(SIMDE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/XenonRecomp/thirdparty/simde")
if(NOT EXISTS "${SIMDE_INCLUDE_DIR}/simde/simde-common.h" AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/simde/simde/simde-common.h")
    set(SIMDE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/simde")
endif()
target_include_directories(ppc_recomp ${ppc_scope}
    "${MW2_PPC_DIR}"
    "${SIMDE_INCLUDE_DIR}"
    "${SIMDE_INCLUDE_DIR}/simde")
if(NOT MW2_SPLIT STREQUAL "runtime")
    # The generated code is machine output; its warnings are noise. Guest arithmetic
    # wraps as the console's does: without -fwrapv a mulld that overflows a signed
    # register is undefined, and the compiler miscompiles it.
    target_compile_options(ppc_recomp PRIVATE -w -fwrapv ${MW2_ARCH_OPTIONS})
    # The console's hardware registers are plain memory in this address space. The
    # recompiler marks the stores that address them (eieio follows), and this header
    # routes those through a hook so the XMA decoder sees its kick, lock and clear
    # registers written. Force-included: ppc/ is regenerated by build.sh.
    target_compile_options(ppc_recomp PRIVATE -include "${CMAKE_SOURCE_DIR}/runtime/mmio_hook.h")
endif()
if((MW2_IS_COD4 OR MW2_IS_AW OR MW2_IS_BO3) AND NOT MW2_SPLIT STREQUAL "runtime")
    # The instructions the recompiler's configuration replaces by calls
    # (config/COD4.toml, config/AW.toml, config/BO3.toml), beside the code
    # that calls them.
    target_sources(ppc_recomp PRIVATE "${CMAKE_SOURCE_DIR}/runtime/recomp/ppc_extra_instructions.cpp")
endif()
if(NOT MW2_LOGGING)
    # Compiles every LOGI/LOGW/LOGE/LOGK call to nothing, warnings included.
    add_compile_definitions(MW2_NO_LOGGING=1)
endif()

if(MW2_TRACE_INDIRECT)
    target_compile_definitions(ppc_recomp ${ppc_scope} MW2_TRACE_INDIRECT=1)
endif()

if(MW2_SPLIT STREQUAL "game")
    include(cmake/game_library.cmake)
    return()
endif()

# MW3's generated PPC can be compiled independently before its runtime hook
# addresses have been identified. This deliberately stops before compiling
# MW2-specific runtime sources or linking an executable with fabricated hooks.
if(MW3_RECOMP_ONLY AND NOT MW3_LINK_PROBE)
    if(NOT MW2_IS_MW3)
        message(FATAL_ERROR "MW3_RECOMP_ONLY requires -DMW2_TITLE=mw3 or mw3mp")
    endif()

    # Compile the shared kernel implementation as object files against the
    # generated MW3 headers. This catches C++ integration errors in import
    # handlers without linking an executable or claiming MW2 semantics are
    # compatible with MW3. MW3-specific hooks and title globals remain gated.
    file(GLOB MW3_KERNEL_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_SOURCE_DIR}/runtime/kernel/*.cpp")
    # The runtime's own sources, which name the title through runtime/title.h:
    # compiling them against MW3's block checks that every entry the code needs
    # is there or compiled out. gpu/, apu/, install/ and online/ come too, with
    # placeholder hashes and playlist data (nothing is linked, so the values are
    # never used). The Vulkan renderer and the XMA decoder are added only where
    # their headers are installed, so a machine with them checks more.
    file(GLOB MW3_TITLE_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_SOURCE_DIR}/runtime/*.cpp"
         "${CMAKE_SOURCE_DIR}/runtime/gpu/*.cpp"
         "${CMAKE_SOURCE_DIR}/runtime/apu/*.cpp"
         "${CMAKE_SOURCE_DIR}/runtime/install/*.cpp"
         "${CMAKE_SOURCE_DIR}/runtime/online/*.cpp")
    list(REMOVE_ITEM MW3_TITLE_SOURCES "${CMAKE_SOURCE_DIR}/runtime/apu/xma.cpp")
    set(MW3_OPTIONAL_NOTES "")
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        file(GLOB MW3_VULKAN_SOURCES CONFIGURE_DEPENDS
             "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/*.cpp")
        list(REMOVE_ITEM MW3_VULKAN_SOURCES
             "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/shader_build.cpp")
        list(APPEND MW3_TITLE_SOURCES ${MW3_VULKAN_SOURCES})
        string(APPEND MW3_OPTIONAL_NOTES " vulkan")
    endif()
    find_path(MW3_AVCODEC_DIR libavcodec/avcodec.h
              HINTS "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-xenia")
    if(MW3_AVCODEC_DIR)
        list(APPEND MW3_TITLE_SOURCES "${CMAKE_SOURCE_DIR}/runtime/apu/xma.cpp")
        string(APPEND MW3_OPTIONAL_NOTES " xma")
    endif()
    message(STATUS "MW3 compile check: optional sources:${MW3_OPTIONAL_NOTES}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/mw3-check-generated")
    file(WRITE "${CMAKE_BINARY_DIR}/mw3-check-generated/playlists_data.h"
         "// Placeholder for the MW3 compile check only.\nstatic const unsigned char kPlaylists[] = { 0 };\nstatic constexpr unsigned kPlaylistsSize = 1;\n")
    add_library(mw3-runtime-compile-check OBJECT ${MW3_KERNEL_SOURCES} ${MW3_TITLE_SOURCES})
    target_include_directories(mw3-runtime-compile-check PRIVATE
        "${MW2_PPC_DIR}"
        "${CMAKE_SOURCE_DIR}/runtime"
        "${CMAKE_BINARY_DIR}/mw3-check-generated"
        "${SIMDE_INCLUDE_DIR}")
    if(MW3_AVCODEC_DIR)
        target_include_directories(mw3-runtime-compile-check PRIVATE "${MW3_AVCODEC_DIR}")
    endif()
    if(Vulkan_FOUND)
        target_link_libraries(mw3-runtime-compile-check PRIVATE Vulkan::Headers)
    endif()
    target_compile_options(mw3-runtime-compile-check PRIVATE
        ${MW2_ARCH_OPTIONS} -fwrapv)
    target_compile_definitions(mw3-runtime-compile-check PRIVATE
        MW3_TITLE=1 MW2_DIAGNOSTICS=0
        MW2_XEX_SHA256_SP=\"compile-check\" MW2_XEX_SHA256_MP=\"compile-check\"
        MW2_DISC_SHA256_SP=\"compile-check\" MW2_DISC_SHA256_MP=\"compile-check\")

    add_custom_target(mw3-recomp-check)
    add_dependencies(mw3-recomp-check ppc_recomp mw3-runtime-compile-check)
    if(BUILD_TESTING)
        add_dependencies(mw3-recomp-check xex-header-test guest-format-test dpc-queue-test)
    endif()
    return()
endif()

if(MW3_LINK_PROBE AND NOT MW2_IS_MW3)
    message(FATAL_ERROR "MW3_LINK_PROBE requires -DMW2_TITLE=mw3 or mw3mp")
endif()

# ---- runtime ---------------------------------------------------------------
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/runtime/*.cpp"
     "${CMAKE_SOURCE_DIR}/runtime/kernel/*.cpp"
     "${CMAKE_SOURCE_DIR}/runtime/gpu/*.cpp"
     "${CMAKE_SOURCE_DIR}/runtime/apu/*.cpp"
     "${CMAKE_SOURCE_DIR}/runtime/install/*.cpp"
     "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/*.cpp")
# The disc image's reader, which the installer shares: the file layer reads the
# game out of an image where it lies (runtime/kernel/volume.h).
list(APPEND RUNTIME_SOURCES "${CMAKE_SOURCE_DIR}/launcher/disc.cpp")

# What this build's shaders come out as (runtime/gpu/vulkan/shader_build.h):
# a SHA-256 of the sources that decide it, the translator's and the pipeline
# layout's, and the launcher's reader of which shaders the game has. The
# compile pass's cache is for one of these, and a player's copy compiles
# again when it changes.
file(GLOB MW2_SHADER_BUILD_INPUTS RELATIVE "${CMAKE_SOURCE_DIR}" CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/shader_translator.*"
     "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/spirv.*"
     "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/bindings.h"
     "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/xenos_ucode.h"
     "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/shader_build.*"
     "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/pipeline.*"
     "${CMAKE_SOURCE_DIR}/launcher/precompile.*"
     "${CMAKE_SOURCE_DIR}/launcher/zone/*.cpp"
     "${CMAKE_SOURCE_DIR}/launcher/zone/*.h"
     "${CMAKE_SOURCE_DIR}/launcher/zone/defs/*.h"
     "${CMAKE_SOURCE_DIR}/launcher/zone/defs/*.txt"
     "${CMAKE_SOURCE_DIR}/launcher/zone/defs/assets/*.txt")
list(SORT MW2_SHADER_BUILD_INPUTS)
set(shader_build_text "")
foreach(input ${MW2_SHADER_BUILD_INPUTS})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${input}")
    file(SHA256 "${CMAKE_SOURCE_DIR}/${input}" input_hash)
    string(APPEND shader_build_text "${input} ${input_hash}\n")
endforeach()
string(SHA256 MW2_SHADER_BUILD "${shader_build_text}")
set_source_files_properties("${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/shader_build.cpp" PROPERTIES COMPILE_DEFINITIONS
    "MW2_SHADER_BUILD=\"${MW2_SHADER_BUILD}\"")

if(ANDROID)
    # The window, the touch pad, the AAudio sink, the driver loader and the
    # JNI surface the app talks to (docs/android.md).
    file(GLOB ANDROID_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/android/*.cpp")
    list(APPEND RUNTIME_SOURCES ${ANDROID_SOURCES})
    # Its processor check is x86 only (__builtin_cpu_supports).
    list(REMOVE_ITEM RUNTIME_SOURCES "${CMAKE_SOURCE_DIR}/runtime/cpu_level.cpp")
    # What loads the recompiled code's library, where it is one (MW2_SPLIT).
    if(NOT MW2_SPLIT STREQUAL "runtime")
        list(REMOVE_ITEM RUNTIME_SOURCES "${CMAKE_SOURCE_DIR}/runtime/android/game_library.cpp")
    endif()
    # The title-update package reader (dependency-free STFS): the TU6 apps
    # apply the update on the device after copying the disc.
    list(APPEND RUNTIME_SOURCES "${CMAKE_SOURCE_DIR}/launcher/package.cpp")
endif()
if(ANDROID OR MW2_TITLE STREQUAL "awmp17" OR MW2_IS_BO3)
    # XenonRecomp's patcher, which turns the disc's executables into the
    # update's. Present when the tree was recompiled on this machine (build.sh
    # clones it); absent for a bare checkout, where TU6 apps report that the
    # APK must come from CI instead of failing to link.
    set(XENON "${CMAKE_SOURCE_DIR}/XenonRecomp")
    if(EXISTS "${XENON}/XenonUtils/xex_patcher.cpp" AND EXISTS "${XENON}/XenonUtils/xex_patcher.h")
        enable_language(C)
        # The patcher itself is runtime/install/xex_patcher_moves.cpp, among
        # the runtime's sources above: it includes XenonUtils/xex_patcher.cpp
        # where MW2_HAVE_XEX_PATCHER says the patcher is here.
        list(APPEND RUNTIME_SOURCES
            "${XENON}/XenonUtils/memory_mapped_file.cpp"
            "${XENON}/thirdparty/libmspack/libmspack/mspack/lzxd.c"
            "${XENON}/thirdparty/tiny-AES-c/aes.c")
        set(MW2_XENON_INCLUDES
            "${XENON}/XenonUtils"
            "${XENON}/thirdparty/simde"
            "${XENON}/thirdparty/libmspack/libmspack/mspack"
            "${XENON}/thirdparty/tiny-AES-c"
            "${XENON}/thirdparty/TinySHA1")
    elseif(MW2_TITLE STREQUAL "awmp17")
        # It makes its executable from the disc's and the update's patch at
        # every start (runtime/install/install.cpp).
        message(FATAL_ERROR "awmp17 needs XenonRecomp/ for its patcher (setup/setup.ps1 or android/fetch_deps.sh clones it)")
    endif()
endif()

# The glue build.sh generates from the recompiled output -- a stub for every
# kernel import the runtime lacks, a tracing hook for every D3D9 entry point --
# lives beside that output, since it is different for each title.
# ---- online service --------------------------------------------------------
# What release this is: the release workflow gives the tag. The game plays only
# with copies of the same release (runtime/version.cpp), and the launcher looks
# for a newer one; a build without a tag plays with other such builds and does
# not update.
set(MW2_RELEASE_TAG "" CACHE STRING "The release tag this build is published as, e.g. v0.3.0")
set_source_files_properties("${CMAKE_SOURCE_DIR}/runtime/version.cpp" PROPERTIES COMPILE_DEFINITIONS
    "MW2_RELEASE_TAG=\"${MW2_RELEASE_TAG}\"")
# What the title's network calls reach (runtime/online/service.h). Every build
# has every backend, and the player chooses one as the game starts (MW2_NETWORK).
set(MW2_ONLINE_SOURCES
    "${CMAKE_SOURCE_DIR}/runtime/online/service.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/online/players.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/online/lan.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/online/server.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/online/steam.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/online/demonware.cpp")

# On Android there is no process of ours to be the executable: the app's
# activity is the process, and this is what it loads (System.loadLibrary,
# runtime/android/jni.cpp). Everywhere else it is the program itself. Its
# name is MW2_LIBRARY_NAME above.
if(ANDROID)
    add_library(mw2 SHARED ${RUNTIME_SOURCES}
        ${MW2_ONLINE_SOURCES}
        "${MW2_PPC_DIR}/stubs_generated.cpp")
else()
    add_executable(mw2 ${RUNTIME_SOURCES}
        ${MW2_ONLINE_SOURCES}
        "${MW2_PPC_DIR}/stubs_generated.cpp")
endif()
set(MW2_OUTPUT_NAME "${MW2_LIBRARY_NAME}")
set_target_properties(mw2 PROPERTIES OUTPUT_NAME "${MW2_OUTPUT_NAME}")
target_link_libraries(mw2 PRIVATE ppc_recomp)
target_compile_options(mw2 PRIVATE ${MW2_ARCH_OPTIONS})
if(MW2_X86_LEVEL STREQUAL "x86-64")
    # Explicit baseline diagnostic builds use SIMDe's portable fallbacks.
    set_property(SOURCE "${CMAKE_SOURCE_DIR}/runtime/cpu_level.cpp"
                 APPEND PROPERTY COMPILE_DEFINITIONS MW2_BASELINE_CPU=1)
endif()
# The one file that must run on any x86-64 processor: it checks for the others.
# Compiled to machine code at once, so the link-time optimiser cannot mix it
# with code of the higher level.
set_source_files_properties("${CMAKE_SOURCE_DIR}/runtime/cpu_level.cpp" PROPERTIES COMPILE_OPTIONS "-march=x86-64;-fno-lto")
if(MW2_PGO_TRAINING)
    # The runtime leaves with _exit, which writes no profile: this writes it first.
    target_sources(mw2 PRIVATE "${CMAKE_SOURCE_DIR}/runtime/pgo/training.cpp")
    target_link_options(mw2 PRIVATE -Wl,--wrap=_exit)
endif()
if(MW2_DIAGNOSTICS)
    target_compile_definitions(mw2 PRIVATE MW2_DIAGNOSTICS=1)
else()
    target_compile_definitions(mw2 PRIVATE MW2_DIAGNOSTICS=0)
endif()
target_include_directories(mw2 PRIVATE "${CMAKE_SOURCE_DIR}/runtime")

# What Find Game offers where no relay hands it out: the relay's own file for
# the title, so a release has one list.
if(MW2_TITLE STREQUAL "mw3mp")
    set(playlists_path "${CMAKE_SOURCE_DIR}/relay/playlists_mw3.info")
elseif(MW2_TITLE STREQUAL "cod4mp")
    set(playlists_path "${CMAKE_SOURCE_DIR}/relay/playlists_cod4.info")
elseif(MW2_TITLE STREQUAL "awmp" OR MW2_TITLE STREQUAL "awmp17")
    set(playlists_path "${CMAKE_SOURCE_DIR}/relay/playlists_aw.info")
else()
    set(playlists_path "${CMAKE_SOURCE_DIR}/relay/playlists.info")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${playlists_path}")
file(READ "${playlists_path}" playlists_hex HEX)
string(LENGTH "${playlists_hex}" playlists_length)
math(EXPR playlists_size "${playlists_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," playlists_bytes "${playlists_hex}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/generated/playlists_data.h" CONTENT
     "// Generated by CMake from ${playlists_path}.\nstatic const unsigned char kPlaylists[] = { ${playlists_bytes} };\nstatic constexpr unsigned kPlaylistsSize = ${playlists_size};\n")
target_include_directories(mw2 PRIVATE "${CMAKE_BINARY_DIR}/generated")
# A game executable runs only the title executable it was recompiled from, and
# the launcher installs only the disc it is an update of: each known by its
# SHA-256, read here from mw2/ (runtime/install/install.cpp, launcher/setup.cpp).
function(mw2_sha256 variable path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${path} is missing -- build.sh makes it from the ISO and the title update")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" hash)
    set(${variable} "${hash}" PARENT_SCOPE)
endfunction()
if(MW2_IS_MW3)
    # An MW3 build runs the executable it was recompiled from, and the app
    # that carries both titles installs and checks both: each is the one in
    # mw3/tu0 when it is there (tools/check_mw3_xex.py has already accepted it
    # before any code was generated), -DMW3_XEX_SHA256_SP/MP for a build that
    # has the generated sources but not the disc, or the first executable the
    # bring-up was done on. MW2_XEX_SHA256_SP/MP are not read here: the Android
    # build always passes MW2's.
    if(MW2_IS_COD4)
        set(mw3_xex_dir "${CMAKE_SOURCE_DIR}/cod4/tu0")
        set(mw3_xex_hash_SP "f6b848ac4ecf2b12881223e4c3cd63c49ae36e1fadc6f2179428033274602739")
        set(mw3_xex_hash_MP "9aae8db5b38f0b394f9052d1ee9c8b1f18038ff86fc5f4110f173f66043a0cc6")
    elseif(MW2_IS_AW)
        set(mw3_xex_dir "${CMAKE_SOURCE_DIR}/aw/tu0")
        set(mw3_xex_hash_SP "a8b1e99c27d6d03a0b0700e6eada1e7d24ee4b194e3c9fa844f16c25f8376a73")
        set(mw3_xex_hash_MP "3ef96359f96ee7994e43de70f56a9c2a019d5b841f11318ca2dffdb717b265f8")
    elseif(MW2_IS_BO3)
        # Its two executables are the multiplayer (default.xex) and the
        # zombies (default_zm.xex): the first has the place the other titles'
        # campaign has here, the second the multiplayer's. These are the
        # disc's, which a player has; the runtime applies title update 5's
        # patches to them at every start and runs what that makes, which is
        # what was recompiled (runtime/install/install.cpp,
        # docs/BO3-BRINGUP.md).
        set(mw3_xex_dir "${CMAKE_SOURCE_DIR}/bo3/tu0")
        set(mw3_xex_hash_SP "e64ed5fa25ca8c97d3970efc9b0d17bc32253cc43dbf9f5f96637aada064d8bb")
        set(mw3_xex_hash_MP "d0eb63c97f69cf14ecaf5171429fa4afcce6f0fd5ca3d774c7a0af343f33fe4e")
    else()
        set(mw3_xex_dir "${CMAKE_SOURCE_DIR}/mw3/tu0")
        set(mw3_xex_hash_SP "d90bc8d93b7b8f6001ead8330271196a6ef7a5c2e4a9e9b63118f47d58a05912")
        set(mw3_xex_hash_MP "0d7669844122b8f94e29ec87baf6bf0a7b84ab4abad8a82b43aa444682f90f7c")
    endif()
    set(mw3_xex_file_SP "default.xex")
    if(MW2_IS_BO3)
        set(mw3_xex_file_MP "default_zm.xex")
    else()
        set(mw3_xex_file_MP "default_mp.xex")
    endif()
    foreach(which SP MP)
        if(MW3_XEX_SHA256_${which})
            set(mw3_xex_hash_${which} "${MW3_XEX_SHA256_${which}}")
        elseif(EXISTS "${mw3_xex_dir}/${mw3_xex_file_${which}}")
            mw2_sha256(mw3_xex_hash_${which} "${mw3_xex_dir}/${mw3_xex_file_${which}}")
        endif()
    endforeach()
    set(MW2_HASH_DEFINITIONS
        MW2_XEX_SHA256_SP="${mw3_xex_hash_SP}"
        MW2_XEX_SHA256_MP="${mw3_xex_hash_MP}"
        MW2_DISC_SHA256_SP="${mw3_xex_hash_SP}"
        MW2_DISC_SHA256_MP="${mw3_xex_hash_MP}")
elseif(MW2_XEX_SHA256_SP AND MW2_XEX_SHA256_MP)
    # CMake's regexes have no {n} repetition -- "{64}" would be matched as
    # those four characters -- so the length is counted separately.
    foreach(given SP MP)
        set(hash "${MW2_XEX_SHA256_${given}}")
        string(LENGTH "${hash}" hash_length)
        if(NOT hash_length EQUAL 64 OR NOT hash MATCHES "^[0-9a-fA-F]+$")
            message(FATAL_ERROR
                "MW2_XEX_SHA256_${given} is not a sha256 (64 hex characters, got"
                " ${hash_length}): ${hash}")
        endif()
    endforeach()
    if(NOT MW2_DISC_SHA256_SP)
        set(MW2_DISC_SHA256_SP "${MW2_XEX_SHA256_SP}")
        set(MW2_DISC_SHA256_MP "${MW2_XEX_SHA256_MP}")
    endif()
    set(MW2_HASH_DEFINITIONS
        MW2_XEX_SHA256_SP="${MW2_XEX_SHA256_SP}" MW2_XEX_SHA256_MP="${MW2_XEX_SHA256_MP}"
        MW2_DISC_SHA256_SP="${MW2_DISC_SHA256_SP}" MW2_DISC_SHA256_MP="${MW2_DISC_SHA256_MP}")
    # The TU fastfiles a TU6 install verifies its downloaded update against.
    # TU6 CI passes all three; without them a TU6 build refuses to apply the
    # update on the device (empty define) rather than trusting any bytes.
    foreach(_ff PATCH_MP DLC1_UI_MP DLC2_UI_MP)
        if(MW2_UPDATE_SHA256_${_ff})
            list(APPEND MW2_HASH_DEFINITIONS MW2_UPDATE_SHA256_${_ff}="${MW2_UPDATE_SHA256_${_ff}}")
        else()
            list(APPEND MW2_HASH_DEFINITIONS MW2_UPDATE_SHA256_${_ff}="")
        endif()
    endforeach()
    message(STATUS "XEX hashes given on the command line; mw2/ is not needed")
else()
    if(MW2_VERSION STREQUAL "tu0")
        set(MW2_XEX_DIR "${CMAKE_SOURCE_DIR}/mw2/tu0")
    else()
        set(MW2_XEX_DIR "${CMAKE_SOURCE_DIR}/mw2")
    endif()
    if(EXISTS "${MW2_XEX_DIR}/default.xex")
        mw2_sha256(MW2_XEX_SHA256_SP  "${MW2_XEX_DIR}/default.xex")
        mw2_sha256(MW2_XEX_SHA256_MP  "${MW2_XEX_DIR}/default_mp.xex")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/mw2/default.xex")
        mw2_sha256(MW2_XEX_SHA256_SP  "${CMAKE_SOURCE_DIR}/mw2/default.xex")
        mw2_sha256(MW2_XEX_SHA256_MP  "${CMAKE_SOURCE_DIR}/mw2/default_mp.xex")
    else()
        set(MW2_XEX_SHA256_SP "990e640d3a92544f9d585e71faf1ab65082d37ec6f5865b2599335fa123bf6ff")
        set(MW2_XEX_SHA256_MP "aacc47aff3221f3bd9129e80827fd3bcd74aa43bf74bb5c5ddbd0597c0d5fcde")
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/mw2/tu0/default.xex")
        mw2_sha256(MW2_DISC_SHA256_SP "${CMAKE_SOURCE_DIR}/mw2/tu0/default.xex")
        mw2_sha256(MW2_DISC_SHA256_MP "${CMAKE_SOURCE_DIR}/mw2/tu0/default_mp.xex")
    else()
        set(MW2_DISC_SHA256_SP "${MW2_XEX_SHA256_SP}")
        set(MW2_DISC_SHA256_MP "${MW2_XEX_SHA256_MP}")
    endif()
    set(MW2_HASH_DEFINITIONS
        MW2_XEX_SHA256_SP="${MW2_XEX_SHA256_SP}" MW2_XEX_SHA256_MP="${MW2_XEX_SHA256_MP}"
        MW2_DISC_SHA256_SP="${MW2_DISC_SHA256_SP}" MW2_DISC_SHA256_MP="${MW2_DISC_SHA256_MP}")
    if(NOT MW2_VERSION STREQUAL "tu0" AND EXISTS "${CMAKE_SOURCE_DIR}/mw2/update/patch_mp.ff")
        mw2_sha256(MW2_UPDATE_SHA256_PATCH_MP   "${CMAKE_SOURCE_DIR}/mw2/update/patch_mp.ff")
        mw2_sha256(MW2_UPDATE_SHA256_DLC1_UI_MP "${CMAKE_SOURCE_DIR}/mw2/update/dlc1_ui_mp.ff")
        mw2_sha256(MW2_UPDATE_SHA256_DLC2_UI_MP "${CMAKE_SOURCE_DIR}/mw2/update/dlc2_ui_mp.ff")
        list(APPEND MW2_HASH_DEFINITIONS
            MW2_UPDATE_SHA256_PATCH_MP="${MW2_UPDATE_SHA256_PATCH_MP}"
            MW2_UPDATE_SHA256_DLC1_UI_MP="${MW2_UPDATE_SHA256_DLC1_UI_MP}"
            MW2_UPDATE_SHA256_DLC2_UI_MP="${MW2_UPDATE_SHA256_DLC2_UI_MP}")
    endif()
endif()
target_compile_definitions(mw2 PRIVATE ${MW2_HASH_DEFINITIONS})
# XenonRecomp's patcher, compiled in on Android where its checkout is there.
if(MW2_XENON_INCLUDES)
    target_include_directories(mw2 PRIVATE ${MW2_XENON_INCLUDES})
    target_compile_definitions(mw2 PRIVATE MW2_HAVE_XEX_PATCHER=1)
endif()
if(WIN32)
    # One executable with nothing beside it: the C++ runtime and the threads
    # linked in. The recompiled code nests deep on the host stack, and a thread
    # Windows starts gets the size the executable asks for: 16 MB, where Linux
    # gives 8.
    target_link_libraries(mw2 PRIVATE ws2_32 iphlpapi winmm bcrypt)
    target_link_options(mw2 PRIVATE -static -Wl,--stack,16777216)
    # A diagnostics build times the ring consumer's thread with clock_gettime
    # (runtime/gpu/command_processor.cpp), which mingw-w64 keeps in
    # winpthreads; the driver links that only when asked. Static, as the rest.
    if(MW2_DIAGNOSTICS)
        target_link_libraries(mw2 PRIVATE pthread)
    endif()
    # A player's build opens no console window (runtime/platform.cpp).
    if(NOT MW2_DIAGNOSTICS)
        target_link_options(mw2 PRIVATE -mwindows)
    endif()
elseif(ANDROID)
    include(cmake/android.cmake)
else()
    # The sub_XXXXXXXX guest functions exported so backtrace_symbols can name
    # them, and nothing else (runtime/exports.list).
    target_link_options(mw2 PRIVATE
        -Wl,--dynamic-list=${CMAKE_CURRENT_SOURCE_DIR}/runtime/exports.list)
    set_property(TARGET mw2 APPEND PROPERTY LINK_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/exports.list)
    if(MW2_PORTABLE)
        target_link_options(mw2 PRIVATE -static-libstdc++ -static-libgcc)
    endif()
endif()

if(MW2_USE_SDL)
    include(FetchContent)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.8
        GIT_SHALLOW TRUE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL3)
    target_link_libraries(mw2 PRIVATE SDL3::SDL3-static)
    target_compile_definitions(mw2 PRIVATE MW2_USE_SDL=1)
endif()

# ---- XMA decoder ----------------------------------------------------------
add_subdirectory(third_party/ffmpeg-xenia)
target_link_libraries(mw2 PRIVATE ffmpeg_xma)

# ---- Vulkan ----------------------------------------------------------------
# Loaded through the loader rather than linked to a driver, so a machine without
# a device still builds and runs; the presenter falls back to headless.
if(WIN32)
    include(cmake/vulkan-windows.cmake)
    add_dependencies(mw2 vulkan_import_library)
elseif(ANDROID)
    # The NDK's sysroot carries the headers and a libvulkan.so, but this build
    # links neither: which Vulkan the process talks to is decided while it
    # runs -- the system's, or a driver the player imported
    # (runtime/gpu/vulkan/loader.cpp, runtime/android/driver.cpp). Setting
    # this here keeps the Vulkan block below as it is.
    set(Vulkan_FOUND TRUE)
    set(Vulkan_LIBRARY "loaded at run time")
else()
    find_package(Vulkan)
endif()
if(Vulkan_FOUND)
    if(NOT ANDROID)
        target_link_libraries(mw2 PRIVATE Vulkan::Vulkan)
    endif()
    target_compile_definitions(mw2 PRIVATE MW2_HAVE_VULKAN=1)
    message(STATUS "Vulkan found: ${Vulkan_LIBRARY}")
else()
    message(STATUS "Vulkan not found -- building without a window")
endif()

if(NOT WIN32 AND NOT ANDROID)
    target_link_libraries(mw2 PRIVATE pthread)
endif()

# The two tools below are for working on the renderer, on Linux.
if(NOT WIN32 AND NOT ANDROID)
# ---- offline shader translation -------------------------------------------
# The translator has to live in the runtime, because the title hands the GPU
# microcode while it runs. Building it as a tool too gives a fast loop against
# captured shaders and spirv-val, without launching the game.
add_executable(translate-shader
    "${CMAKE_SOURCE_DIR}/tools/translate_shader.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/shader_translator.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/pipeline.cpp"
    # The device this tool brings up is the runtime's own, so it answers for the
    # same Snapdragon workaround (bc_decoder.cpp, vk::bc::AdrenoMode).
    "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/bc_decoder.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/spirv.cpp")
target_include_directories(translate-shader PRIVATE "${CMAKE_SOURCE_DIR}/runtime")
# --compile puts a translated module in front of the real driver, which is a
# stronger check than spirv-val: it is the compiler that would run the shader.
if(Vulkan_FOUND)
    target_link_libraries(translate-shader PRIVATE Vulkan::Vulkan)
    target_compile_definitions(translate-shader PRIVATE MW2_HAVE_VULKAN=1)
endif()

# ---- offline texture upload ------------------------------------------------
# The upload hands the driver the guest's own block bytes, so reading the image
# back returns exactly what went in. That is a test, and it needs neither the
# game nor a picture -- unlike the untiler, which needs both.
add_executable(upload-texture
    "${CMAKE_SOURCE_DIR}/tools/upload_texture.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/texture.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/texture_cache.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/bc_decoder.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/image_memory.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/texture_formats.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/pipeline.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/shader_translator.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/spirv.cpp")
target_include_directories(upload-texture PRIVATE "${CMAKE_SOURCE_DIR}/runtime")
if(Vulkan_FOUND)
    target_link_libraries(upload-texture PRIVATE Vulkan::Vulkan)
    target_compile_definitions(upload-texture PRIVATE MW2_HAVE_VULKAN=1)
endif()

# ---- the software decoder's speed ------------------------------------------
# Not a check: a number. The decoder runs on every texture uploaded where the
# driver cannot sample block compression, so the loops in it are worth timing
# the way the comments there quote -- a 2048x2048 decode, per format, with the
# output buffer reused as the runtime reuses its scratch.
add_executable(bc-bench
    "${CMAKE_SOURCE_DIR}/tools/bc_bench.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/bc_decoder.cpp")
target_include_directories(bc-bench PRIVATE "${CMAKE_SOURCE_DIR}/runtime")
if(Vulkan_FOUND)
    target_link_libraries(bc-bench PRIVATE Vulkan::Vulkan)
    target_compile_definitions(bc-bench PRIVATE MW2_HAVE_VULKAN=1)
endif()
endif()

# ---- the checks `ctest` runs -----------------------------------------------
# The three of them are defined in a project of their own (tests/CMakeLists.txt)
# so they can be configured and run on a tree that cannot build the runtime: this
# file refuses to configure without the recompiled guest code, which is made from
# the disc's executables and is in no repository, so a CI runner has none of it
# (.github/workflows/tests.yml). Including it here is what keeps a local `ctest`
# running exactly what CI runs.
if(NOT ANDROID)
    add_subdirectory(tests)
endif()

# ---- launcher --------------------------------------------------------------
# What a player opens (launcher/): it installs the game and starts it. One
# executable with nothing beside it, like the game's: Dear ImGui on SDL's own
# renderer, the fonts as arrays, and XenonRecomp's patcher, which applies the
# title update, compiled in from its checkout.
if(MW2_USE_SDL)
    enable_language(C)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.9b
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(imgui)

    # third_party/fonts/*.ttf as C arrays.
    set(MW2_FONTS_SOURCE "${CMAKE_BINARY_DIR}/launcher_fonts.cpp")
    set(fonts_text "// Generated by CMake from third_party/fonts/.\n")
    foreach(font "Menu=BarlowCondensed-Regular" "Heading=BarlowCondensed-Medium" "Body=BarlowSemiCondensed-Regular")
        string(REPLACE "=" ";" font "${font}")
        list(GET font 0 font_name)
        list(GET font 1 font_file)
        set(font_path "${CMAKE_SOURCE_DIR}/third_party/fonts/${font_file}.ttf")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${font_path}")
        file(READ "${font_path}" font_hex HEX)
        string(LENGTH "${font_hex}" font_length)
        math(EXPR font_size "${font_length} / 2")
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," font_bytes "${font_hex}")
        string(APPEND fonts_text "extern const unsigned char kFont${font_name}[] = { ${font_bytes} };\n"
                                 "extern const unsigned kFont${font_name}Size = ${font_size};\n")
    endforeach()
    # The window's icon the same way (tools/launcher_icon.py makes it).
    set(icon_path "${CMAKE_SOURCE_DIR}/launcher/icon/launcher.bmp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${icon_path}")
    file(READ "${icon_path}" icon_hex HEX)
    string(LENGTH "${icon_hex}" icon_length)
    math(EXPR icon_size "${icon_length} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," icon_bytes "${icon_hex}")
    string(APPEND fonts_text "extern const unsigned char kIcon[] = { ${icon_bytes} };\n"
                             "extern const unsigned kIconSize = ${icon_size};\n")
    file(GENERATE OUTPUT "${MW2_FONTS_SOURCE}" CONTENT "${fonts_text}")

    # The game draws the system's screens (runtime/screen.cpp) with the same
    # fonts, rasterised by the stb_truetype that comes with Dear ImGui.
    target_sources(mw2 PRIVATE "${MW2_FONTS_SOURCE}")
    target_include_directories(mw2 PRIVATE "${imgui_SOURCE_DIR}")
    target_compile_definitions(mw2 PRIVATE MW2_SCREENS=1)

    # The fastfile reader (launcher/zone/): zlib for the zone's stream, and
    # the definitions it walks a zone by, embedded as the fonts are.
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    # The game too: the title's XLAST is gzipped in its image (runtime/presence.cpp).
    target_link_libraries(mw2 PRIVATE zlibstatic)
    target_include_directories(mw2 PRIVATE "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
    # The 360's under their own names, the PC game's (defs_pc/, read by the map
    # converter, launcher/mapport/) under pc/.
    set(MW2_ZONE_DEFS_SOURCE "${CMAKE_BINARY_DIR}/launcher_zone_defs.cpp")
    set(zone_text "// Generated by CMake from launcher/zone/defs/ and defs_pc/.\n")
    set(zone_table "")
    set(zone_index 0)
    foreach(zone_set "defs:" "defs_pc:pc/")
        string(REPLACE ":" ";" zone_set "${zone_set}")
        list(GET zone_set 0 zone_folder)
        list(LENGTH zone_set zone_set_length)
        set(zone_prefix "")
        if(zone_set_length GREATER 1)
            list(GET zone_set 1 zone_prefix)
        endif()
        set(zone_defs_dir "${CMAKE_SOURCE_DIR}/launcher/zone/${zone_folder}")
        file(GLOB zone_defs RELATIVE "${zone_defs_dir}" CONFIGURE_DEPENDS
            "${zone_defs_dir}/*.h" "${zone_defs_dir}/*.txt" "${zone_defs_dir}/assets/*.txt")
        foreach(def ${zone_defs})
            file(READ "${zone_defs_dir}/${def}" def_hex HEX)
            string(LENGTH "${def_hex}" def_length)
            math(EXPR def_size "${def_length} / 2")
            string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," def_bytes "${def_hex}")
            string(APPEND zone_text "static const unsigned char kDef${zone_index}[] = { ${def_bytes} };\n")
            string(APPEND zone_table "    { \"${zone_prefix}${def}\", kDef${zone_index}, ${def_size} },\n")
            math(EXPR zone_index "${zone_index} + 1")
        endforeach()
    endforeach()
    string(APPEND zone_text "struct ZoneDefinition { const char* name; const unsigned char* data; unsigned size; };\n"
                            "extern const ZoneDefinition kZoneDefinitions[] = {\n${zone_table}};\n"
                            "extern const unsigned kZoneDefinitionCount = ${zone_index};\n")
    file(GENERATE OUTPUT "${MW2_ZONE_DEFS_SOURCE}" CONTENT "${zone_text}")
    file(GLOB ZONE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/launcher/zone/*.cpp")
    # The map converter (launcher/mapport/): PC maps made into 360 ones.
    file(GLOB MAPPORT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/launcher/mapport/*.cpp")

    set(XENON "${CMAKE_SOURCE_DIR}/XenonRecomp")
    file(GLOB LAUNCHER_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/launcher/*.cpp")
    add_executable(launcher ${LAUNCHER_SOURCES}
        "${MW2_FONTS_SOURCE}"
        ${ZONE_SOURCES} ${MAPPORT_SOURCES} "${MW2_ZONE_DEFS_SOURCE}"
        # The game's own shader build, for the compile pass (launcher/precompile.cpp).
        "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/pipeline.cpp"
        "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/shader_build.cpp"
        "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/shader_marker.cpp"
        "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/shader_translator.cpp"
        "${CMAKE_SOURCE_DIR}/runtime/gpu/vulkan/spirv.cpp"
        "${CMAKE_SOURCE_DIR}/runtime/install/crypto.cpp"
        "${CMAKE_SOURCE_DIR}/runtime/install/xex.cpp"
        "${CMAKE_SOURCE_DIR}/runtime/install/spawn.cpp"
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
        "${CMAKE_SOURCE_DIR}/runtime/install/xex_patcher_moves.cpp"
        "${XENON}/XenonUtils/memory_mapped_file.cpp"
        "${XENON}/thirdparty/libmspack/libmspack/mspack/lzxd.c"
        "${XENON}/thirdparty/tiny-AES-c/aes.c")
    set_target_properties(launcher PROPERTIES OUTPUT_NAME mw2-launcher)
    target_include_directories(launcher PRIVATE
        "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends"
        "${XENON}/XenonUtils" "${XENON}/thirdparty/simde"
        "${XENON}/thirdparty/libmspack/libmspack/mspack"
        "${XENON}/thirdparty/tiny-AES-c" "${XENON}/thirdparty/TinySHA1")
    target_compile_definitions(launcher PRIVATE ${MW2_HASH_DEFINITIONS} NOMINMAX MW2_HAVE_XEX_PATCHER=1)
    # What release this is, for the launcher to look for a newer one (see
    # MW2_RELEASE_TAG above). A build without one does not update.
    if(WIN32)
        set(MW2_RELEASE_KIND "windows")
    else()
        set(MW2_RELEASE_KIND "linux")
    endif()
    set_source_files_properties("${CMAKE_SOURCE_DIR}/launcher/update.cpp" PROPERTIES COMPILE_DEFINITIONS
        "MW2_RELEASE_TAG=\"${MW2_RELEASE_TAG}\";MW2_RELEASE_KIND=\"${MW2_RELEASE_KIND}\"")
    # The decoder the game has for its sound, for the menu sounds (launcher/sound.cpp).
    target_link_libraries(launcher PRIVATE SDL3::SDL3-static ffmpeg_xma zlibstatic)
    if(Vulkan_FOUND)
        target_link_libraries(launcher PRIVATE Vulkan::Vulkan)
        target_compile_definitions(launcher PRIVATE MW2_HAVE_VULKAN=1)
        if(WIN32)
            add_dependencies(launcher vulkan_import_library)
        endif()
    endif()
    target_include_directories(launcher PRIVATE "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
    if(WIN32)
        # The program's icon, as Explorer and the taskbar show it.
        enable_language(RC)
        target_sources(launcher PRIVATE "${CMAKE_SOURCE_DIR}/launcher/icon/launcher.rc")
        target_include_directories(launcher PRIVATE "${CMAKE_SOURCE_DIR}/launcher/compat")
        target_link_libraries(launcher PRIVATE urlmon)
        target_link_options(launcher PRIVATE -static)
        if(NOT MW2_DIAGNOSTICS)
            target_link_options(launcher PRIVATE -mwindows)
        endif()
    else()
        target_link_libraries(launcher PRIVATE pthread)
        if(MW2_PORTABLE)
            target_link_options(launcher PRIVATE -static-libstdc++ -static-libgcc)
        endif()
    endif()

    # The launcher's fastfile reader on its own: every shader of the files it
    # is given, to check the reader against the game's files.
    if(NOT WIN32)
        add_executable(zone-shaders "${CMAKE_SOURCE_DIR}/tools/zone_shaders.cpp"
            ${ZONE_SOURCES} "${MW2_ZONE_DEFS_SOURCE}"
            "${CMAKE_SOURCE_DIR}/runtime/install/xex.cpp" "${CMAKE_SOURCE_DIR}/runtime/install/crypto.cpp")
        target_link_libraries(zone-shaders PRIVATE zlibstatic)
        target_include_directories(zone-shaders PRIVATE "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
    endif()

    # The map converter on its own, and the checks it is built against.
    add_executable(map-port "${CMAKE_SOURCE_DIR}/tools/map_port.cpp"
        ${ZONE_SOURCES} ${MAPPORT_SOURCES} "${MW2_ZONE_DEFS_SOURCE}"
        "${CMAKE_SOURCE_DIR}/runtime/install/xex.cpp" "${CMAKE_SOURCE_DIR}/runtime/install/crypto.cpp")
    target_link_libraries(map-port PRIVATE zlibstatic)
    target_include_directories(map-port PRIVATE "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
    if(NOT WIN32)
        target_link_libraries(map-port PRIVATE pthread)   # (std::async: stock files are read on worker threads)
    endif()
    if(MW2_PORTABLE)
        target_link_options(map-port PRIVATE -static-libstdc++ -static-libgcc)
    endif()
endif()

# ---- mods: the script compiler ---------------------------------------------
# Modern Warfare 3 on this console loads compiled scripts only, so a mod's
# .gsc is compiled before the title sees it (runtime/mods_gsc.cpp), by
# gsc-tool (xensik, GPL-3.0, THIRD_PARTY.md): the compiler, and the tables of
# this build's instruction, function, method and name numbers. Only the
# sources that the Xbox 360 build of IW5 needs are compiled. Advanced
# Warfare's multiplayer as title update 17 makes it uses the same loader for
# the Exo Zombies bots' script (runtime/aw_zombie_bots.cpp), with gsc-tool's
# Xbox 360 build of S1 (its instruction numbers; the built-in functions' are
# runtime/aw_gsc_builtins.h).
set(MW2_GSC_ENGINE "")
if(MW2_IS_MW3 AND NOT MW2_IS_COD4 AND NOT MW2_IS_AW AND NOT MW2_IS_BO3)
    set(MW2_GSC_ENGINE iw5_xb)
elseif(MW2_TITLE STREQUAL "awmp17")
    set(MW2_GSC_ENGINE s1_xb)
endif()
if(MW2_GSC_ENGINE)
    include(FetchContent)
    FetchContent_Declare(gsc_tool
        URL https://github.com/xensik/gsc-tool/archive/6dc1aa3712b8b0bfda23afa304846383c18153d1.tar.gz
        URL_HASH SHA256=e06dbdfe7f21d2cd85190aca635386673303870cd5e060f3725c7e14e9fa9f62)
    # It has no CMakeLists.txt of its own (it builds with premake), so this
    # only fetches it.
    FetchContent_MakeAvailable(gsc_tool)
    file(GLOB GSC_TOOL_SOURCES
        "${gsc_tool_SOURCE_DIR}/src/gsc/*.cpp"
        "${gsc_tool_SOURCE_DIR}/src/gsc/common/*.cpp"
        "${gsc_tool_SOURCE_DIR}/src/gsc/engine/${MW2_GSC_ENGINE}*.cpp")
    add_library(gsc_engine STATIC ${GSC_TOOL_SOURCES}
        "${gsc_tool_SOURCE_DIR}/src/utils/reader.cpp"
        "${gsc_tool_SOURCE_DIR}/src/utils/writer.cpp"
        "${gsc_tool_SOURCE_DIR}/src/utils/string.cpp")
    target_include_directories(gsc_engine PUBLIC "${gsc_tool_SOURCE_DIR}/include")
    target_compile_options(gsc_engine PRIVATE -w ${MW2_ARCH_OPTIONS})
    target_link_libraries(mw2 PRIVATE gsc_engine)
    target_compile_definitions(mw2 PRIVATE MW2_GSC=1)
    # zlib, for the deflated halves of a script file and a raw file: the
    # system's on Android, the launcher's copy on the desktop.
    if(ANDROID)
        target_link_libraries(mw2 PRIVATE z)
    elseif(NOT TARGET zlibstatic)
        find_package(ZLIB REQUIRED)
        target_link_libraries(mw2 PRIVATE ZLIB::ZLIB)
    endif()
endif()
