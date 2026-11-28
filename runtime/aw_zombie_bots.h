#pragma once
// The Exo Zombies bots' script (aw_zombie_bots.cpp): a script of the port's
// own that the mods loader (mods.cpp) compiles and runs beside the game's.
#include <string_view>

namespace awzombiebots
{
    // Its path among the scripts, as a mod's own entry (docs/mods.md).
    inline constexpr std::string_view kPath = "scripts/mp/aw_zombie_bots";
    // Its source; empty when MW2_ZOMBIE_BOTS=0 switched it off.
    std::string_view Script();
}
