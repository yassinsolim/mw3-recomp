#pragma once
// Custom camouflages in Modern Warfare 3's Create a Class (mw3_custom_camos.cpp).
#include <cstdint>

namespace mw3camo
{
    // The menu's getPlayerData has answered (mw3_unlock_all.cpp hooks it):
    // `operands` and `first` are its path and `result` its {type, value}. A
    // class's camouflage that is stored as Gold but was chosen from a pack
    // reads as that pack's entry, so the menus show what was chosen.
    void PlayerDataRead(uint8_t* base, uint32_t operands, uint32_t first, uint32_t result);
}
