#pragma once
// The recompiled code as a library of its own (docs/game-library.md): built
// by the player from their own disc and installed beside the app, which is
// published without it. Compiled only where the build is split (MW2_SPLIT).
#include <ppc_recomp_shared.h>
#include <string>

namespace game
{
    // Loads the library at `path`, checks it was built against what this
    // runtime was, and finds the originals of the hooked functions in it.
    // Empty when it is ready to run; otherwise what is wrong with it, for the
    // app to show, and nothing of it is used.
    std::string Load(const char* path);

    // The library's table of the title's functions, once loaded.
    const PPCFuncMapping* Mappings();
}
