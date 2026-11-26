# The recompiled code as a library of its own, beside the app's (docs/game-library.md).
#
# The app's library (MW2_SPLIT=runtime) holds the runtime and nothing made
# from the game, so it can be published and updated; the recompiled code's
# library (MW2_SPLIT=game) is built by each player from their own disc. The
# two are compiled apart, at different times, and meet in the phone's dynamic
# linker, so each carries a statement of what it was built against, and the
# runtime loads only a game library whose statement is its own
# (runtime/android/game_library.cpp).
#
# The statement says what the library is, a line for each thing: the title,
# the code the recompiler made (the SHA-256 of the recompiled tree: every
# file of it that is compiled, and the three headers they include), the
# code's extent in the image, the register context, the system calls the
# executable imports, and the header compiled into every recompiled file.
# Carriage returns are dropped before hashing: a Windows checkout and the
# recompiler's own output there have them, a Linux runner's have not.
#
# What the code was made *with* (the recompiler, this repository's patch to
# it, the title's configuration, the jump tables) is not in it: a change of
# those that leaves the code as it was leaves the statement as it was, and no
# player builds anything again. They were the statement until November 2026,
# and a library built then says them and not its code; the app takes such a
# library when its statement is one this repository knows to have been made
# from the same code (config/abi/<library>/earlier_statements.list).
#
# Which functions the runtime may replace is not in it, because all of them
# may be: the game library leaves every sub_XXXXXXXX to the dynamic linker
# (cmake/game_library.cmake), so a hook added by an update of the app needs no
# new game library.

if(NOT ANDROID)
    message(FATAL_ERROR "MW2_SPLIT is for the Android app; the desktop game is one executable")
endif()
if(NOT MW2_IS_MW3 AND NOT MW2_VERSION STREQUAL "tu0")
    message(FATAL_ERROR "MW2_SPLIT is for Modern Warfare 2's disc version (tu0): title update 6 is built as one library (docs/MW2-ANDROID.md)")
endif()
if(NOT MW2_SPLIT MATCHES "^(runtime|game)$")
    message(FATAL_ERROR "MW2_SPLIT must be OFF, runtime or game, not '${MW2_SPLIT}'")
endif()

# What a build without the disc knows of the title's recompiled tree
# (config/abi/<library>/): the code's extent and the system calls, which the
# runtime is compiled against, and the tree's SHA-256, recorded from a tree by
# -DMW2_ABI_RECORD=ON below. Numbers and the names of system calls; none of
# the game's code.
set(abi_recorded "${CMAKE_SOURCE_DIR}/config/abi/${MW2_LIBRARY_NAME}")
# The recompiler writes its context header into the tree behind these lines.
set(abi_context_prefix "#pragma once\n#include \"ppc_config.h\"\n\n")

function(mw2_abi_text variable path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${path} is missing: it is part of what the game library is checked against")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" text)
    string(REPLACE "\r" "" text "${text}")
    set(${variable} "${text}" PARENT_SCOPE)
endfunction()
function(mw2_abi_hash variable path)
    mw2_abi_text(text "${path}")
    string(SHA256 hash "${text}")
    set(${variable} "${hash}" PARENT_SCOPE)
endfunction()

# The SHA-256 of a recompiled tree: of a line for each of its files that is
# compiled or included, the file's own SHA-256 and its name, in the order of
# the names. GameBuild.kt (treeHash) makes the same of a tree recompiled on a
# phone, where the files are the PC's byte for byte.
function(mw2_abi_tree variable tree)
    file(GLOB files "${tree}/ppc_recomp.*.cpp")
    list(APPEND files "${tree}/ppc_func_mapping.cpp" "${tree}/ppc_config.h" "${tree}/ppc_context.h"
        "${tree}/ppc_recomp_shared.h")
    set(names "")
    foreach(file ${files})
        get_filename_component(name "${file}" NAME)
        list(APPEND names "${name}")
    endforeach()
    list(SORT names)
    set(lines "")
    foreach(name ${names})
        mw2_abi_hash(hash "${tree}/${name}")
        string(APPEND lines "${hash}  ${name}\n")
    endforeach()
    string(SHA256 hash "${lines}")
    set(${variable} "${hash}" PARENT_SCOPE)
endfunction()

mw2_abi_hash(abi_mmio "${CMAKE_SOURCE_DIR}/runtime/mmio_hook.h")
set(abi_recorded_code "")
if(EXISTS "${abi_recorded}/generated_code.sha256")
    mw2_abi_text(abi_recorded_code "${abi_recorded}/generated_code.sha256")
    string(STRIP "${abi_recorded_code}" abi_recorded_code)
endif()

if(MW2_SPLIT STREQUAL "game")
    mw2_abi_text(abi_extent "${MW2_PPC_DIR}/ppc_config.h")
    mw2_abi_text(abi_imports "${MW2_PPC_DIR}/kernel_imports.txt")
    mw2_abi_hash(abi_context "${MW2_PPC_DIR}/ppc_context.h")
    mw2_abi_tree(abi_code "${MW2_PPC_DIR}")
    # For whoever changes the recompiler or its configuration: what the app's
    # library is then built against, to be committed with that change. The
    # earlier statements are of libraries with the code recorded before, and
    # go when that is no longer this tree's.
    option(MW2_ABI_RECORD "Record this recompiled tree in config/abi/ as what the app expects" OFF)
    if(MW2_ABI_RECORD)
        file(WRITE "${abi_recorded}/ppc_config.h" "${abi_extent}")
        file(WRITE "${abi_recorded}/kernel_imports.list" "${abi_imports}")
        file(WRITE "${abi_recorded}/generated_code.sha256" "${abi_code}\n")
        if(NOT abi_code STREQUAL abi_recorded_code)
            file(REMOVE "${abi_recorded}/earlier_statements.list")
        endif()
        message(STATUS "Game library: recorded ${MW2_PPC_DIR} in ${abi_recorded}")
    elseif(NOT abi_code STREQUAL abi_recorded_code)
        message(WARNING "${MW2_PPC_DIR} is not the code config/abi/${MW2_LIBRARY_NAME} records:"
            " the app built from this checkout will refuse the library. Recompile with this checkout's"
            " recompiler and configuration (setup/setup.ps1 -Recomp); or, when one of those was changed"
            " on purpose, record the tree with -DMW2_ABI_RECORD=ON and commit config/abi/.")
    endif()
else()
    if(NOT abi_recorded_code)
        message(FATAL_ERROR "${abi_recorded}/generated_code.sha256 is missing: it is the code the app takes")
    endif()
    set(abi_code "${abi_recorded_code}")
    mw2_abi_text(abi_extent "${abi_recorded}/ppc_config.h")
    mw2_abi_text(abi_imports "${abi_recorded}/kernel_imports.list")
    set(abi_context_file "${CMAKE_SOURCE_DIR}/XenonRecomp/XenonUtils/ppc_context.h")
    if(NOT EXISTS "${abi_context_file}")
        message(FATAL_ERROR "XenonRecomp/ is missing: the runtime is compiled against its register context."
            " setup/setup.ps1 clones and patches it; so does android/fetch_deps.sh, for a build without the disc.")
    endif()
    mw2_abi_text(abi_context "${abi_context_file}")
    string(SHA256 abi_context "${abi_context_prefix}${abi_context}")
endif()
string(SHA256 abi_extent_hash "${abi_extent}")
string(SHA256 abi_imports_hash "${abi_imports}")

# One line for each thing, so a mismatch can say which of them differs.
set(abi_interface "")
string(APPEND abi_interface
    "code-extent ${abi_extent_hash}\n"
    "register-context ${abi_context}\n"
    "system-calls ${abi_imports_hash}\n"
    "memory-hook ${abi_mmio}\n")
set(MW2_GAME_ABI "title ${MW2_LIBRARY_NAME}\ngenerated-code ${abi_code}\n${abi_interface}")
string(SHA256 abi_short "${MW2_GAME_ABI}")
string(SUBSTRING "${abi_short}" 0 12 abi_short)
message(STATUS "Game library: ${MW2_SPLIT} of ${MW2_LIBRARY_NAME}, built against ${abi_short}")
set(abi_built_against "${MW2_LIBRARY_NAME} ${abi_short}\n")

# The statements of libraries built before the statement named the code: each
# is in config/abi/<library>/earlier_statements.list behind the line
# "generated-code <the SHA-256 of the tree it was made from>", an empty line
# between two. The app takes those made from the code it takes now, with the
# interface it has now; the others are of no use to it and are left out.
set(abi_earlier "")
if(MW2_SPLIT STREQUAL "runtime" AND EXISTS "${abi_recorded}/earlier_statements.list")
    mw2_abi_text(abi_earlier_text "${abi_recorded}/earlier_statements.list")
    string(REGEX REPLACE "(^|\n)#[^\n]*" "" abi_earlier_text "${abi_earlier_text}")
    string(REGEX REPLACE "\n\n+" ";" abi_earlier_text "${abi_earlier_text}")
    foreach(block ${abi_earlier_text})
        string(STRIP "${block}" block)
        string(FIND "${block}\n" "generated-code ${abi_code}\ntitle ${MW2_LIBRARY_NAME}\n" at_start)
        string(FIND "${block}\n" "\n${abi_interface}" at_end REVERSE)
        string(LENGTH "${block}\n" length)
        string(LENGTH "\n${abi_interface}" interface_length)
        math(EXPR expected_end "${length} - ${interface_length}")
        if(at_start EQUAL 0 AND at_end EQUAL expected_end)
            string(REGEX REPLACE "^generated-code [^\n]*\n" "" statement "${block}\n")
            string(APPEND abi_earlier "    R\"abi(${statement})abi\",\n")
            string(SHA256 earlier_short "${statement}")
            string(SUBSTRING "${earlier_short}" 0 12 earlier_short)
            string(APPEND abi_built_against "${MW2_LIBRARY_NAME} ${earlier_short}\n")
        endif()
    endforeach()
endif()

function(mw2_write_if_different path text)
    if(EXISTS "${path}")
        file(READ "${path}" old)
        if(old STREQUAL text)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${text}")
endfunction()

# What this build is, in a line: for setup to say, and for a release to
# publish beside the app so that setup can tell a game library built from
# another state of the repository before it is on the phone. The app's has a
# line more for each earlier statement it takes.
mw2_write_if_different("${CMAKE_BINARY_DIR}/game_abi.txt" "${abi_built_against}")

if(MW2_SPLIT STREQUAL "game")
    # The statement, as a symbol the runtime reads before it uses anything else
    # of the library.
    set(MW2_GAME_ABI_SOURCE "${CMAKE_BINARY_DIR}/game_abi.cpp")
    mw2_write_if_different("${MW2_GAME_ABI_SOURCE}"
        "// Generated by cmake/game_abi.cmake.\nextern \"C\" __attribute__((visibility(\"default\"))) const char mw2_game_abi[] = R\"abi(${MW2_GAME_ABI})abi\";\n")
    return()
endif()

# ---- the runtime's stand-in for the recompiled tree --------------------------
# The runtime's sources include the tree's headers and link its stubs
# (CMakeLists.txt, MW2_PPC_DIR). Here they are made from what is recorded: the
# code's extent as it is, the recompiler's context header, a shared header
# that declares the system calls and none of the title's functions (the
# runtime reaches those through runtime/title.h), and a stub for every system
# call, weak, so that the runtime's own definition of one is the one linked.
set(MW2_PPC_DIR "${CMAKE_BINARY_DIR}/game-interface")
file(MAKE_DIRECTORY "${MW2_PPC_DIR}")
mw2_write_if_different("${MW2_PPC_DIR}/ppc_config.h" "${abi_extent}")
mw2_write_if_different("${MW2_PPC_DIR}/ppc_context.h"
    "${abi_context_prefix}#include \"${abi_context_file}\"\n")
string(REGEX MATCHALL "[A-Za-z0-9_]+" abi_import_names "${abi_imports}")
set(abi_declarations "")
set(abi_stubs "")
foreach(name ${abi_import_names})
    string(APPEND abi_declarations "PPC_FUNC(__imp__${name});\n")
    string(APPEND abi_stubs
        "__attribute__((weak)) PPC_FUNC(__imp__${name}) { kernel::Unimplemented(\"${name}\"); ctx.r3.u64 = 0; }\n")
endforeach()
mw2_write_if_different("${MW2_PPC_DIR}/ppc_recomp_shared.h"
    "// Generated by cmake/game_abi.cmake.\n#pragma once\n#include \"ppc_context.h\"\n\nextern \"C\" {\n${abi_declarations}}\n")
mw2_write_if_different("${MW2_PPC_DIR}/stubs_generated.cpp"
    "// Generated by cmake/game_abi.cmake: every system call the executable imports\n// logs once and returns 0 unless the runtime implements it (tools/gen_kernel_stubs.py\n// is this for a build with the recompiled tree).\n#include <ppc_recomp_shared.h>\n#include \"kernel/kernel.h\"\n\n${abi_stubs}")
mw2_write_if_different("${CMAKE_BINARY_DIR}/generated/game_abi_expected.h"
    "// Generated by cmake/game_abi.cmake.\nstatic const char kExpectedGameAbi[] = R\"abi(${MW2_GAME_ABI})abi\";\n// Statements of libraries built from the same code before the statement named it.\nstatic const char* const kEarlierGameAbis[] = {\n${abi_earlier}    nullptr,\n};\n")
