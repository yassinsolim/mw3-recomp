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
# The statement is the SHA-256 of everything that decides the shape of the
# recompiled code and of what it calls: the recompiler (its commit and this
# repository's patch to it), the title's recompiler configuration, its jump
# tables, the code's extent in the image, the register context, the system
# calls the executable imports and the header compiled into every recompiled
# file. Carriage returns are dropped before hashing: a Windows checkout and
# the recompiler's own output there have them, a Linux runner's have not.
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

# The title's recompiler configuration and jump tables, and what a build
# without the disc knows of its recompiled tree (config/abi/<library>/): the
# three facts of it the runtime is compiled against, recorded from a tree by
# -DMW2_ABI_RECORD=ON below. Numbers and the names of system calls; none of
# the game's code.
if(MW2_TITLE STREQUAL "awmp")
    set(abi_toml "${CMAKE_SOURCE_DIR}/config/AWMP.toml")
    set(abi_tables "${CMAKE_SOURCE_DIR}/config/awmp_switch_tables.toml")
elseif(MW2_TITLE STREQUAL "aw")
    set(abi_toml "${CMAKE_SOURCE_DIR}/config/AW.toml")
    set(abi_tables "${CMAKE_SOURCE_DIR}/config/aw_switch_tables.toml")
elseif(MW2_TITLE STREQUAL "cod4mp")
    set(abi_toml "${CMAKE_SOURCE_DIR}/config/COD4MP.toml")
    set(abi_tables "${CMAKE_SOURCE_DIR}/config/cod4mp_switch_tables.toml")
elseif(MW2_TITLE STREQUAL "cod4")
    set(abi_toml "${CMAKE_SOURCE_DIR}/config/COD4.toml")
    set(abi_tables "${CMAKE_SOURCE_DIR}/config/cod4_switch_tables.toml")
elseif(MW2_TITLE STREQUAL "mw3mp")
    set(abi_toml "${CMAKE_SOURCE_DIR}/config/MW3MP.toml")
    set(abi_tables "${CMAKE_SOURCE_DIR}/config/mw3mp_switch_tables.toml")
elseif(MW2_TITLE STREQUAL "mw3")
    set(abi_toml "${CMAKE_SOURCE_DIR}/config/MW3.toml")
    set(abi_tables "${CMAKE_SOURCE_DIR}/config/mw3_switch_tables.toml")
elseif(MW2_TITLE STREQUAL "mp")
    set(abi_toml "${CMAKE_SOURCE_DIR}/config/MW2MP.tu0.toml")
    set(abi_tables "${CMAKE_SOURCE_DIR}/config/mw2mp_switch_tables.tu0.toml")
else()
    set(abi_toml "${CMAKE_SOURCE_DIR}/config/MW2.tu0.toml")
    set(abi_tables "${CMAKE_SOURCE_DIR}/config/mw2_switch_tables.tu0.toml")
endif()
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

# The recompiler's commit, from the one script every player's build starts with.
mw2_abi_text(abi_setup "${CMAKE_SOURCE_DIR}/setup/setup.ps1")
if(NOT abi_setup MATCHES "XenonCommit[ ]*=[ ]*'([0-9a-f]+)'")
    message(FATAL_ERROR "setup/setup.ps1 no longer names the XenonRecomp commit ($XenonCommit)")
endif()
set(abi_commit "${CMAKE_MATCH_1}")
mw2_abi_hash(abi_patch "${CMAKE_SOURCE_DIR}/patches/xenonrecomp-mw2.patch")
mw2_abi_hash(abi_config "${abi_toml}")
mw2_abi_hash(abi_shared "${CMAKE_SOURCE_DIR}/tools/genshared.py")
mw2_abi_hash(abi_mmio "${CMAKE_SOURCE_DIR}/runtime/mmio_hook.h")
# What a tree recompiled now would be made with: the recompiler, its
# configuration, and the script that writes the tree's shared header.
set(abi_recompiled "")
string(APPEND abi_recompiled
    "recompiler ${abi_commit}\n"
    "recompiler-patch ${abi_patch}\n"
    "configuration ${abi_config}\n"
    "shared-header ${abi_shared}\n")

if(MW2_SPLIT STREQUAL "game")
    mw2_abi_hash(abi_switch "${abi_tables}")
    mw2_abi_text(abi_extent "${MW2_PPC_DIR}/ppc_config.h")
    mw2_abi_text(abi_imports "${MW2_PPC_DIR}/kernel_imports.txt")
    mw2_abi_hash(abi_context "${MW2_PPC_DIR}/ppc_context.h")
    # The tree says what it was made with (setup/setup.ps1 writes the note as
    # it recompiles), and that is what the library states: a tree older than
    # this checkout makes a library the app refuses, not one that claims to
    # be current and is not. A tree without the note is one of build.sh's or
    # an older setup's; -DMW2_ABI_TRUST_TREE=ON vouches that it was made from
    # this checkout as it stands.
    option(MW2_ABI_TRUST_TREE "The recompiled tree has no note of what it was made with, and is current" OFF)
    if(EXISTS "${MW2_PPC_DIR}/recompiled_with.txt")
        mw2_abi_text(abi_tree "${MW2_PPC_DIR}/recompiled_with.txt")
        if(NOT abi_tree STREQUAL abi_recompiled)
            message(WARNING "${MW2_PPC_DIR} was recompiled with an older recompiler or configuration than this checkout's:"
                " the app built from this checkout will refuse the library. Recompile (setup/setup.ps1 -Recomp).")
        endif()
        set(abi_recompiled "${abi_tree}")
    elseif(NOT MW2_ABI_TRUST_TREE)
        message(FATAL_ERROR "${MW2_PPC_DIR} does not say what it was recompiled with (recompiled_with.txt)."
            " Recompile it with setup/setup.ps1 -Recomp, or pass -DMW2_ABI_TRUST_TREE=ON if it was made from this checkout as it is.")
    endif()
    # For whoever changes the recompiler or its configuration: what the app's
    # library is then built against, to be committed with that change.
    option(MW2_ABI_RECORD "Record this recompiled tree in config/abi/ as what the app expects" OFF)
    if(MW2_ABI_RECORD)
        file(WRITE "${abi_recorded}/ppc_config.h" "${abi_extent}")
        file(WRITE "${abi_recorded}/kernel_imports.list" "${abi_imports}")
        file(WRITE "${abi_recorded}/switch_tables.sha256" "${abi_switch}\n")
        message(STATUS "Game library: recorded ${MW2_PPC_DIR} in ${abi_recorded}")
    endif()
else()
    mw2_abi_text(abi_switch "${abi_recorded}/switch_tables.sha256")
    string(STRIP "${abi_switch}" abi_switch)
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
set(MW2_GAME_ABI "")
string(APPEND MW2_GAME_ABI
    "title ${MW2_LIBRARY_NAME}\n"
    "${abi_recompiled}"
    "jump-tables ${abi_switch}\n"
    "code-extent ${abi_extent_hash}\n"
    "register-context ${abi_context}\n"
    "system-calls ${abi_imports_hash}\n"
    "memory-hook ${abi_mmio}\n")
string(SHA256 abi_short "${MW2_GAME_ABI}")
string(SUBSTRING "${abi_short}" 0 12 abi_short)
message(STATUS "Game library: ${MW2_SPLIT} of ${MW2_LIBRARY_NAME}, built against ${abi_short}")

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
# another state of the repository before it is on the phone.
mw2_write_if_different("${CMAKE_BINARY_DIR}/game_abi.txt" "${MW2_LIBRARY_NAME} ${abi_short}\n")

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
    "// Generated by cmake/game_abi.cmake.\nstatic const char kExpectedGameAbi[] = R\"abi(${MW2_GAME_ABI})abi\";\n")
