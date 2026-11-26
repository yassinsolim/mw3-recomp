#pragma once
// The maps of a player's own map packs in Modern Warfare 3's multiplayer menus (mw3_dlc_maps.cpp).
#include <ppc_recomp_shared.h>

#include <cstdint>

namespace mw3maps
{
    // A menu was added to the title's list (mw3_custom_camos.cpp hooks that):
    // `menu` is its menuDef_t and `name` its name. The packs' map list is
    // made high enough for its maps.
    void MenuAdded(uint8_t* base, uint32_t menu, const char* name);
    // A level's fastfile is loaded and its scripts are about to be (mods.cpp
    // hooks that): a pack's map is given the teams its fastfile holds.
    void LevelLoading(PPCContext& ctx, uint8_t* base);
}
