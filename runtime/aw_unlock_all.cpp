// MW2_UNLOCK_ALL=1: every item of Advanced Warfare's multiplayer reads as
// unlocked, as mw3_unlock_all.cpp does for Modern Warfare 3. Nothing is
// written to the stats: the title's own answers to "is this locked" are
// replaced, and the rank and the challenges stay what the player earned.
//
// One function answers for a row of mp/unlocktable.csv, sub_82495080
// (controller, stats, table, row, int* needed). It reads the row's type
// (column 1) and says why the row is locked, or 0:
//
//     "title", "icon"   the titleUnlocked / iconUnlocked bit, else the row's
//                       challenge (column 7), else the rank of column 2
//                       against mp/rankTable.csv: 2, with the rank in *needed
//     "loot"            what a supply drop gave (sub_82793198): 5
//     anything else     the prestige of column 15: 13; the challenge of column
//                       3, "attachmentUnlockSpecial" included: 1
//
// Its six callers are all the title has: the menus' checks through
// sub_82496258, the menu's line saying what an item needs (sub_82495680, the
// user of LUA_MENU_ITEM_LEVEL_UNLOCK_DESC and _PRESTIGE_UNLOCK_DESC), the
// answer with the experience still missing (sub_82495958), the server's check
// of a client's item (sub_824F2C98, which reads the server's clients) and the
// two that copy the answers into the profile's bits (sub_825EC5D8,
// sub_825EC768).
//
// sub_82496258(controller, table, row) is the menus' way in (the Lua
// IsItemUnlocked, sub_82719128, reaches it through sub_823CA2B8, which finds
// the row by the item's name): it answers 0 for a row whose column 21 names a
// dvar that is set, fetches the controller's stats, and answers 3 when there
// are none, so it is hooked as well.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "env.h"
#include "log.h"

#include <cstdint>

#if defined(T_Unlock_RowLocked) && defined(T_Unlock_RowLockedForController)

namespace
{
    bool UnlockAll()
    {
        static const bool on = [] {
            const bool value = env::Flag("MW2_UNLOCK_ALL");
            if (value) LOGI("unlock: MW2_UNLOCK_ALL: every multiplayer item reads as unlocked; the stats are not changed");
            return value;
        }();
        return on;
    }
}

GUEST_HOOK(T_Unlock_RowLocked)
{
    if (!UnlockAll()) { GUEST_ORIG(T_Unlock_RowLocked)(ctx, base); return; }
    // What the item needs, for the menu's "unlocked at ..." line: nothing.
    if (ctx.r7.u32) PPC_STORE_U32(ctx.r7.u32, 0);
    ctx.r3.u64 = 0;
}

GUEST_HOOK(T_Unlock_RowLockedForController)
{
    if (!UnlockAll()) { GUEST_ORIG(T_Unlock_RowLockedForController)(ctx, base); return; }
    ctx.r3.u64 = 0;
}

#endif
