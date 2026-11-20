# The recompiled code's library, lib<title>game.so (MW2_SPLIT=game,
# docs/game-library.md): ppc_recomp as a shared library, with the statement
# of what it was built against (cmake/game_abi.cmake) and nothing of the
# runtime. setup/setup.ps1 builds it from the player's own disc and packages
# it as an app with no code of its own (android/game), which the game's app
# loads it from.

set_target_properties(ppc_recomp PROPERTIES OUTPUT_NAME "${MW2_TITLE}game")

# As the app's library is linked (cmake/android.cmake): 16 KB pages, frame
# pointers for the crash report's backtrace, a build id.
target_link_options(ppc_recomp PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(ppc_recomp PRIVATE -fno-omit-frame-pointer)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(ppc_recomp PRIVATE -Wl,--build-id=sha1)
endif()

# The recompiler emits each of the title's functions twice: __imp__sub_X, the
# code, and sub_X, a weak function that only calls it, which every caller and
# the function table name. In one library the runtime's own sub_X (a hook,
# GUEST_HOOK in runtime/title.h) replaces the weak one at the link. Here the
# phone's dynamic linker does that when the library is loaded: the runtime's
# library is in the global group (-z global, cmake/android.cmake), which is
# searched before this library's own symbols, for every symbol left to it.
#
# The dynamic list is what leaves them to it: sub_* and nothing else. Without
# it every symbol of a shared library can be replaced, and each sub_X's jump
# to its own __imp__sub_X would go through the linker's table as well, twice
# the cost for nothing: nobody replaces an __imp__sub_X. With it the calls to
# sub_X, and the function table's entries, are bound when the library is
# loaded, to the runtime's function where it has one and to this library's
# otherwise, and everything else is bound here and now.
target_link_options(ppc_recomp PRIVATE
    "-Wl,--dynamic-list=${CMAKE_SOURCE_DIR}/cmake/game_library.list")
set_property(TARGET ppc_recomp APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/cmake/game_library.list")

# The system calls, the bad-indirect-call report, the timebase and the XMA
# register hook are the runtime's, which is loaded already when this library
# is: undefined here, and not an error. No library is named as needed for
# them, because the runtime's is not the same file for every app that could
# load this one, and the global group is where they are found anyway.
target_link_options(ppc_recomp PRIVATE -Wl,--unresolved-symbols=ignore-all)
