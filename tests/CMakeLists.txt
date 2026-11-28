# The checks `ctest` runs: the durable write, the log's repeat counting, the
# software block-compressed decoder and the DemonWare lobby stand-in. They are
# defined in a project of their own
# so they can be configured on a tree that cannot build the runtime -- the root
# CMakeLists refuses to configure without the recompiled guest code, which is
# made from the disc's executables and is in no repository, so a CI runner has
# none of it (.github/workflows/tests.yml). The root includes this same file,
# so a local `ctest` runs exactly what CI runs:
#
#   cmake -S tests -B build-tests -G Ninja
#   ctest --test-dir build-tests --output-on-failure
#
# Nothing here needs a disc, a game file, a GPU or the guest image: each check
# depends on one source file and the headers around it.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(mw2checks CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release)
    endif()
    enable_testing()
endif()

set(MW2_RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/../runtime")

# bc_decoder.cpp includes <vulkan/vulkan.h> for the format constants it answers
# with. It opens no device and calls into no driver, so the headers are all it
# needs; Windows has none to find and fetches them as the root build does. The
# root build has already done this when it includes this file, hence the test
# for the target rather than for the platform alone.
if(NOT TARGET Vulkan::Vulkan)
    if(WIN32)
        include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/vulkan-windows.cmake")
    else()
        find_package(Vulkan)
    endif()
endif()

# ---- durable writes --------------------------------------------------------
# Unlike the tools in ../tools, which are harnesses to be read, this asserts:
# a save survives losing the process part-way, a replacement leaves the previous
# contents whole, a successful write leaves no `.tmp`, one left by a dead run is
# not mistaken for the target, and sizes round-trip across block boundaries.
add_executable(durable-test
    "${CMAKE_CURRENT_SOURCE_DIR}/durable_test.cpp"
    "${MW2_RUNTIME}/kernel/durable.cpp")
target_include_directories(durable-test PRIVATE "${MW2_RUNTIME}")
add_test(NAME durable COMMAND durable-test)

# ---- the log's repeat counting ---------------------------------------------
# The rule that keeps a run readable when it logs one thing a thousand times
# (runtime/log.h): a line is written as it doubles with the count beside it, two
# lines differing only by an address are one kind, and a repetition in progress
# cannot flood.
add_executable(log-test
    "${CMAKE_CURRENT_SOURCE_DIR}/log_repeat_test.cpp")
target_include_directories(log-test PRIVATE "${MW2_RUNTIME}")
add_test(NAME log COMMAND log-test)

# ---- the software block-compressed decoder ---------------------------------
# What a GPU without native BC support samples (bc_decoder.cpp), checked against
# a decoder written from the formats themselves -- so a wrong index bit order,
# an endpoint swap or a row stride fails here rather than looking like a bad
# texture on one phone.
add_executable(bc-test
    "${CMAKE_CURRENT_SOURCE_DIR}/bc_decoder_test.cpp"
    "${MW2_RUNTIME}/gpu/vulkan/bc_decoder.cpp")
target_include_directories(bc-test PRIVATE "${MW2_RUNTIME}")
if(TARGET Vulkan::Vulkan)
    target_link_libraries(bc-test PRIVATE Vulkan::Vulkan)
    target_compile_definitions(bc-test PRIVATE MW2_HAVE_VULKAN=1)
endif()
add_test(NAME bc COMMAND bc-test)

# ---- volume textures -------------------------------------------------------
# Where every level of a 3D texture lives in guest memory (runtime/gpu/texture.cpp),
# checked against Xenia's GetGuestTextureLayout transcribed for the comparison:
# the spans, the level offsets, and that untiling a volume reads each level from
# its own place. The depth term of a volume's level stride is what the code
# called unchecked.
add_executable(volume-test
    "${CMAKE_CURRENT_SOURCE_DIR}/texture_volume_test.cpp"
    "${MW2_RUNTIME}/gpu/texture.cpp")
target_include_directories(volume-test PRIVATE "${MW2_RUNTIME}")
add_test(NAME volume COMMAND volume-test)

# ---- release tag order -----------------------------------------------------
# launcher/version.h decides whether a release is newer than the running build
# (launcher/update.cpp). A tag it cannot read means the launcher never offers
# that release -- which is what happened to this project's own -alpha tags --
# so the order and the refusals are checked here, with no network or launcher.
add_executable(version-test
    "${CMAKE_CURRENT_SOURCE_DIR}/version_test.cpp")
target_include_directories(version-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../launcher")
add_test(NAME version COMMAND version-test)

# ---- what a run's first frames look like -----------------------------------
# The three numbers the app judges a driver's first frames by
# (runtime/gpu/vulkan/picture_probe.h): one colour, noise, a picture. The
# presenter's copy needs a device; what is made of the copy does not.
add_executable(picture-probe-test
    "${CMAKE_CURRENT_SOURCE_DIR}/picture_probe_test.cpp")
target_include_directories(picture-probe-test PRIVATE "${MW2_RUNTIME}/gpu/vulkan")
add_test(NAME picture-probe COMMAND picture-probe-test)

# ---- how the frame sits on the panel ---------------------------------------
# The swapchain's transform and what the log says of it
# (runtime/gpu/vulkan/surface_transform.h): the warning that the picture may be
# rotated only where the surface refuses identity.
add_executable(surface-transform-test
    "${CMAKE_CURRENT_SOURCE_DIR}/surface_transform_test.cpp")
target_include_directories(surface-transform-test PRIVATE "${MW2_RUNTIME}/gpu/vulkan")
add_test(NAME surface-transform COMMAND surface-transform-test)

# ---- what the driver self-test asks of a driver -----------------------------
# Its pixel shaders (runtime/gpu/vulkan/selftest.cpp): each the texture
# indexing of one form the renderer's own take, nothing more.
add_executable(selftest-shaders-test
    "${CMAKE_CURRENT_SOURCE_DIR}/selftest_shaders_test.cpp")
target_include_directories(selftest-shaders-test PRIVATE "${MW2_RUNTIME}/gpu/vulkan")
add_test(NAME selftest-shaders COMMAND selftest-shaders-test)

# ---- what a device sustains ------------------------------------------------
# The rule that holds a run to the frame rate its device keeps and gives it
# the sun's shadows when its frames have room for them (runtime/adaptive.h):
# checked on windows of play as a phone would measure them, since the phones
# it is for -- every chip but the two the project owns -- are not here.
add_executable(adaptive-test
    "${CMAKE_CURRENT_SOURCE_DIR}/adaptive_test.cpp"
    "${MW2_RUNTIME}/adaptive_governor.cpp")
target_include_directories(adaptive-test PRIVATE "${MW2_RUNTIME}")
add_test(NAME adaptive COMMAND adaptive-test)

# ---- the DemonWare lobby stand-in ------------------------------------------
# What MW3's multiplayer logs on to instead of its publisher's servers
# (runtime/online/demonware.cpp): the stream's framing, the player's files read
# back whole and kept in the player's own folder, the playlists as the
# aggregate the title inflates, and that bytes which are not the protocol end
# the connection or the task without a read past them or a file written.
add_executable(demonware-test
    "${CMAKE_CURRENT_SOURCE_DIR}/demonware_test.cpp"
    "${MW2_RUNTIME}/online/demonware.cpp"
    "${MW2_RUNTIME}/kernel/durable.cpp")
target_include_directories(demonware-test PRIVATE "${MW2_RUNTIME}" "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
add_test(NAME demonware COMMAND demonware-test)

# ---- the pipeline cache between runs ---------------------------------------
# Whether the driver's pipeline cache is kept for the next run, by GPU and Mali
# driver release, and that a blob read back reaches the driver only when its
# VkPipelineCacheHeaderVersionOne names this device
# (runtime/gpu/vulkan/pipeline_cache_rules.h): no Mali phone is here to say so.
add_executable(pipeline-cache-test
    "${CMAKE_CURRENT_SOURCE_DIR}/pipeline_cache_test.cpp")
target_include_directories(pipeline-cache-test PRIVATE "${MW2_RUNTIME}")
add_test(NAME pipeline-cache COMMAND pipeline-cache-test)

if(WIN32)
    # The same rule the game is linked with: no C++ runtime beside the
    # executable. Without it a check that pulls in one more libstdc++ symbol
    # than the shell's PATH can supply -- a std::filesystem one, say -- fails at
    # process start (0xc0000139) instead of failing on the code under test, and
    # a check that never starts looks like a check that never ran.
    target_link_options(durable-test PRIVATE -static)
    target_link_options(log-test PRIVATE -static)
    target_link_options(bc-test PRIVATE -static)
    target_link_options(volume-test PRIVATE -static)
    target_link_options(version-test PRIVATE -static)
    target_link_options(picture-probe-test PRIVATE -static)
    target_link_options(selftest-shaders-test PRIVATE -static)
    target_link_options(adaptive-test PRIVATE -static)
    target_link_options(demonware-test PRIVATE -static)
    target_link_options(pipeline-cache-test PRIVATE -static)
endif()
