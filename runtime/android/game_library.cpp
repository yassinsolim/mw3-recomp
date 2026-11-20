// The recompiled code as a library of its own (docs/game-library.md).
//
// The app is published without the game's code: that is made from the
// player's disc, on the player's PC, as lib<title>game.so in a package of its
// own. This is where the two meet. The game library names the runtime's
// symbols -- the system calls, and every sub_X, which the dynamic linker
// binds to the runtime's hook where there is one (cmake/game_library.cmake)
// -- and the runtime finds the three kinds of thing it needs of the game
// library here, by name, after loading it: the statement of what it was
// built against, the function table, and the original of each hooked
// function (runtime/title.h, GUEST_ORIGINAL).
//
// A library built against anything else is not used. An app update that
// changes the recompiler, its configuration or the headers the recompiled
// code is compiled with changes the statement, and the player is told to
// build the game library again rather than left with a crash somewhere in it.
#include "game_library.h"
#include "title.h"
#include "guest_memory.h"
#include "log.h"
#include <game_abi_expected.h>

#include <cstring>
#include <dlfcn.h>
#include <sstream>

namespace
{
    // The originals declared across the runtime: each adds itself here as the
    // runtime's own library is loaded, long before the game's.
    game::Original* g_originals = nullptr;
    const PPCFuncMapping* g_mappings = nullptr;

    // The first thing the two statements disagree about, by its name.
    std::string Difference(const char* expected, const char* found)
    {
        std::istringstream ours(expected), theirs(found);
        std::string mine, other;
        while (std::getline(ours, mine))
        {
            if (!std::getline(theirs, other)) other.clear();
            if (mine != other) return mine.substr(0, mine.find(' '));
        }
        return "an addition";
    }
}

game::Original::Original(const char* symbol) : symbol(symbol), next(g_originals)
{
    g_originals = this;
}

PPCFunc* game::Function(uint32_t address)
{
    return PPC_LOOKUP_FUNC(guest::Base(), address);
}

const PPCFuncMapping* game::Mappings() { return g_mappings; }

std::string game::Load(const char* path)
{
    void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library)
    {
        // A library that names a symbol this runtime has not got ends here,
        // with the linker's sentence saying which.
        const char* why = dlerror();
        LOGE("game library: %s would not load: %s", path, why ? why : "?");
        return why ? why : "the system would not load it";
    }
    const auto refuse = [&](const std::string& why)
    {
        LOGE("game library: %s refused: %s", path, why.c_str());
        dlclose(library);
        return why;
    };

    const char* abi = static_cast<const char*>(dlsym(library, "mw2_game_abi"));
    if (!abi) return refuse("it does not say what it was built against");
    if (std::strcmp(abi, kExpectedGameAbi) != 0)
    {
        LOGE("game library: this app expects\n%s", kExpectedGameAbi);
        LOGE("game library: the library was built against\n%s", abi);
        return refuse("its " + Difference(kExpectedGameAbi, abi) + " is not the one this app was built for");
    }

    const auto* mappings = static_cast<const PPCFuncMapping*>(dlsym(library, "PPCFuncMappings"));
    if (!mappings) return refuse("it has no function table");
    for (Original* original = g_originals; original; original = original->next)
    {
        original->function = reinterpret_cast<PPCFunc*>(dlsym(library, original->symbol));
        if (!original->function) return refuse(std::string("it has no ") + original->symbol);
    }
    g_mappings = mappings;
    LOGI("game library: %s", path);
    return {};
}
