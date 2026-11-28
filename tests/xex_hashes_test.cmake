# The executables a disc must have are named in more than one file, and a disc
# that one accepts and another refuses reaches the player as a refusal the
# app's own check never gave (the installer inside the runtime is told the
# hashes by CMakeLists.txt; the Android app's check of a disc is
# android/app/build.gradle.kts's kit table; a release or a test build is told
# MW3's by the workflow and by tools/release/update_test_apks.sh). This reads
# those places and fails when they differ:
#
#   cmake -P tests/xex_hashes_test.cmake
#
# It needs no disc, no game file and no build: only the five files' text.
cmake_minimum_required(VERSION 3.20)

get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(read_text variable relative)
    file(READ "${root}/${relative}" text)
    set(${variable} "${text}" PARENT_SCOPE)
endfunction()

read_text(cmake "CMakeLists.txt")
read_text(gradle "android/app/build.gradle.kts")
read_text(workflow ".github/workflows/android-release.yml")
read_text(script "tools/release/update_test_apks.sh")

# Told from inside the helpers, so kept where every scope sees it.
function(problem text)
    set_property(GLOBAL APPEND PROPERTY problems "\n  ${text}")
endfunction()

# The hash a pattern's first group captured in [text], which must be one
# sha256; empty (with a problem told) when it is not there.
function(capture variable text pattern what)
    string(REGEX MATCH "${pattern}" found "${text}")
    set(hash "${CMAKE_MATCH_1}")
    string(LENGTH "${hash}" length)
    if(NOT found OR NOT length EQUAL 64)
        problem("${what}: no sha256 found by ${pattern}")
        set(hash "")
    endif()
    set(${variable} "${hash}" PARENT_SCOPE)
endfunction()

# Every hash [pattern] captures in [text], in the order they come.
function(capture_all variable text pattern what expected)
    string(REGEX MATCHALL "${pattern}" found "${text}")
    set(hashes "")
    foreach(one IN LISTS found)
        string(REGEX MATCH "${pattern}" ignored "${one}")
        string(LENGTH "${CMAKE_MATCH_1}" length)
        if(NOT length EQUAL 64)
            problem("${what}: '${CMAKE_MATCH_1}' is not a sha256")
        endif()
        list(APPEND hashes "${CMAKE_MATCH_1}")
    endforeach()
    list(LENGTH hashes count)
    if(NOT count EQUAL expected)
        problem("${what}: ${count} hashes found by ${pattern}, ${expected} expected")
    endif()
    set(${variable} "${hashes}" PARENT_SCOPE)
endfunction()

function(same what a b)
    if(NOT "${a}" STREQUAL "${b}")
        problem("${what}: ${a} is not ${b}")
    endif()
endfunction()

# CMakeLists.txt's defaults, in the order its branches come: Call of Duty 4,
# Advanced Warfare, Black Ops III, Modern Warfare 3 (campaign, then the
# multiplayer; Black Ops III's second is the zombies').
capture_all(cmake_defaults "${cmake}" "set\\(mw3_xex_hash_[SM]P \"([0-9a-f]+)\"\\)" "CMakeLists.txt MW3-family defaults" 8)
list(GET cmake_defaults 0 cod4_sp)
list(GET cmake_defaults 1 cod4_mp)
list(GET cmake_defaults 2 aw_sp)
list(GET cmake_defaults 3 aw_mp)
list(GET cmake_defaults 4 bo3_sp)
list(GET cmake_defaults 5 bo3_mp)
list(GET cmake_defaults 6 mw3_sp)
list(GET cmake_defaults 7 mw3_mp)
capture(mw2_sp "${cmake}" "set\\(MW2_XEX_SHA256_SP \"([0-9a-f]+)\"\\)" "CMakeLists.txt MW2 default (campaign)")
capture(mw2_mp "${cmake}" "set\\(MW2_XEX_SHA256_MP \"([0-9a-f]+)\"\\)" "CMakeLists.txt MW2 default (multiplayer)")

# The app's check of a disc, per title of the kit.
foreach(entry
        "mw2|default.xex|${mw2_sp}" "mw2mp|default_mp.xex|${mw2_mp}"
        "cod4|default.xex|${cod4_sp}" "cod4mp|default_mp.xex|${cod4_mp}"
        "aw|default.xex|${aw_sp}" "awmp|default_mp.xex|${aw_mp}")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 library)
    list(GET parts 1 file)
    list(GET parts 2 wanted)
    string(REPLACE "." "\\." file_pattern "${file}")
    capture(kit "${gradle}" "KitTitle\\(\"${library}\", \"${file_pattern}\", listOf\\(\"([0-9a-f]+)\"\\)" "the kit's ${library}")
    same("the kit's ${library} (${file}) and CMakeLists.txt's" "${kit}" "${wanted}")
endforeach()

# Black Ops III's titles are checked on the disc's executables, which the
# kit calls discSha256 and CMakeLists.txt has as the executables it runs.
capture_all(kit_bo3 "${gradle}" "discSha256 = listOf\\(\"([0-9a-f]+)\"\\)" "the kit's Black Ops III" 2)
list(GET kit_bo3 0 kit_bo3_sp)
list(GET kit_bo3 1 kit_bo3_mp)
same("the kit's bo3 and CMakeLists.txt's" "${kit_bo3_sp}" "${bo3_sp}")
same("the kit's bo3zm and CMakeLists.txt's" "${kit_bo3_mp}" "${bo3_mp}")

# Modern Warfare 2's hashes, which the Android build passes to CMake.
capture(gradle_mw2_sp "${gradle}" "setting\\(\"MW2_XEX_SHA256_SP\"\\) \\?: \"([0-9a-f]+)\"" "build.gradle.kts MW2 default (campaign)")
capture(gradle_mw2_mp "${gradle}" "setting\\(\"MW2_XEX_SHA256_MP\"\\) \\?: \"([0-9a-f]+)\"" "build.gradle.kts MW2 default (multiplayer)")
same("MW2 (campaign), build.gradle.kts and CMakeLists.txt" "${gradle_mw2_sp}" "${mw2_sp}")
same("MW2 (multiplayer), build.gradle.kts and CMakeLists.txt" "${gradle_mw2_mp}" "${mw2_mp}")

# Modern Warfare 3: what the kit is told when nothing else is given is what
# CMake falls back to, and is what a release is built with. The bring-up's
# other packaging (tools/check_mw3_xex.py) is in none of them.
capture(kit_mw3_sp "${gradle}" "val mw3ReleaseExecutableSha256 = \"([0-9a-f]+)\"" "build.gradle.kts MW3 campaign")
capture(kit_mw3_mp "${gradle}" "val mw3ReleaseExecutableMpSha256 = \"([0-9a-f]+)\"" "build.gradle.kts MW3 multiplayer")
same("MW3 campaign, build.gradle.kts and CMakeLists.txt" "${kit_mw3_sp}" "${mw3_sp}")
same("MW3 multiplayer, build.gradle.kts and CMakeLists.txt" "${kit_mw3_mp}" "${mw3_mp}")
if(NOT gradle MATCHES "KitTitle\\(\"mw3\", \"default\\.xex\", listOf\\(mw3ExecutableSha256\\)")
    problem("build.gradle.kts: the kit's mw3 no longer takes mw3ExecutableSha256")
endif()
if(NOT gradle MATCHES "KitTitle\\(\"mw3mp\", \"default_mp\\.xex\", listOf\\(mw3ExecutableMpSha256\\)")
    problem("build.gradle.kts: the kit's mw3mp no longer takes mw3ExecutableMpSha256")
endif()
capture(release_workflow "${workflow}" "MW3_XEX_SHA256: ([0-9a-f]+)" "android-release.yml MW3_XEX_SHA256")
capture(release_script "${script}" "MW3_XEX_SHA256=([0-9a-f]+)" "update_test_apks.sh MW3_XEX_SHA256")
same("MW3, the release workflow and CMakeLists.txt" "${release_workflow}" "${mw3_sp}")
same("MW3, update_test_apks.sh and CMakeLists.txt" "${release_script}" "${mw3_sp}")

get_property(problems GLOBAL PROPERTY problems)
if(problems)
    string(REPLACE ";" "" problems "${problems}")
    message(FATAL_ERROR "The files that name the executables a disc must have disagree:${problems}")
endif()
message(STATUS "The executables' hashes agree in CMakeLists.txt, the app's kit, the release workflow and the test-build script")
