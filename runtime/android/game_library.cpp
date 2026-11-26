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
// A library built against anything else is not used. The statement names the
// code itself (the recompiled tree's hash) and what the two halves share
// (cmake/game_abi.cmake), so an update of the app stops the game only when it
// expects other code than the player has: the game code is then built again
// rather than left to crash somewhere inside. A library from before the
// statement named the code is taken when its statement is one known to have
// been made from this same code.
#include "game_library.h"
#include "title.h"
#include "guest_memory.h"
#include "log.h"
#include "crash.h"
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
        std::istringstream ours(expected);
        std::string mine;
        while (std::getline(ours, mine))
        {
            // An earlier library's statement has lines of its own before
            // these: what it differs in is looked for by name.
            const std::string name = mine.substr(0, mine.find(' ') + 1);
            const std::string all = std::string("\n") + found;
            const size_t at = all.find("\n" + name);
            if (at == std::string::npos || all.compare(at + 1, mine.size() + 1, mine + "\n") != 0)
                return name.substr(0, name.size() - 1);
        }
        return "statement";
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
    crash::Step("game-code", "loading the recompiled game code from its package");
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
    bool known = std::strcmp(abi, kExpectedGameAbi) == 0;
    for (const char* const* earlier = kEarlierGameAbis; !known && *earlier; ++earlier)
        known = std::strcmp(abi, *earlier) == 0;
    if (!known)
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
