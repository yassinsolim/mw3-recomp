// MW2_UNLOCK_ALL=1: every item of Modern Warfare 3's multiplayer reads as
// unlocked. Nothing is written to the stats: the title's own answers to "is this
// unlocked" are replaced, and the rank, the weapon levels and the challenges
// stay what the player earned.
//
// One function answers for a row of mp/unlocktable.csv, sub_8228FA68(stats,
// table, row). It reads the row's type (column 1) and decides by it:
//
//     "title", "icon"      the titleUnlocked / iconUnlocked bit, else the row's
//                          challenge (column 7), else rank and challenge as below
//     "weaponAttachment"   the row's name is "iw5_<weapon> <item>" (sub_8228AD68
//     "weaponCamo"         splits it); sub_8228AC18 compares the weapon's
//     "weaponBuff"         weaponRank in the stats with the level
//     "weaponReticle"      mp/weaponRankTable.csv gives the item (columns 2..5)
//     anything else        the rank that "experience" amounts to against column 2,
//                          and the challenge of column 3
//
// All 15 calls of it are covered by hooking it: the menu's isItemUnlocked,
// isCardIconUnlocked, isCardTitleUnlocked and isProfileItemUnlocked
// (sub_822D6B10, sub_822D6D38, sub_822D6E60, sub_822D73B0, through
// sub_8228FE60), the script's isItemUnlocked (sub_82255688), the challenge
// check (sub_8228FFF8), the custom class check (sub_822916A0, sub_82291A88) and
// the two that copy the answers into the profile's bits (sub_82182748,
// sub_821829C0).
//
// sub_8228FE60(controller, table, row) is the menu's way in: it fetches the
// controller's stats and answers 0 when there are none, so it is hooked as
// well. When the stats are not the source (sub_82288968 returns 0) the menu
// reads the profile's bits instead, through three leaves that name the array
// and jump to sub_82182610: "iconUnlocked", "titleUnlocked" and "unlock". Their
// other callers (sub_823C61D8, sub_823C6290) only ask before granting a title
// or an emblem, and grant nothing when told it is there.
//
// The menu's isItemUnlocked already answers 1 for everything in a private match
// (the xblive_privatematch test at 822D6B74), so this is the title's own
// behaviour there, extended to the other menus.
//
// The create-a-class menus do not ask it for what a weapon's level gives (its
// attachments, proficiencies, reticles and camouflages: on the device they
// stayed locked with the hooks above): they compare the weapon's level
// themselves. The executable has no menu function for a weapon's level, so the
// menu reads it as getPlayerData("weaponRank", <weapon>), which is
// sub_822D6120(controller, operands, first, result): operands are 8 bytes
// {type, value} with their count at +0x50 (type 2 a string, sub_822D2DA0), the
// result is {type, value} with type 0 an integer; getPlayerDataSplitScreen
// (sub_822D6258) calls it with first = 1. Only this reader is changed: a path
// starting with "weaponRank" gives the highest level in
// mp/weaponRankTable.csv, whose column 4 is the level an item needs
// (sub_8228AC18 compares the stats' weaponRank with it), and one starting with
// "weaponXP" the largest number in the table.
//
// A match checks the class again. The script that gives a spawning player the
// loadout reads customClasses.<n>.weaponSetups.<n>.{weapon, attachment, buff,
// camo, reticle} through its own getPlayerData (sub_8225A710, the client's
// stats on the server), asks isItemUnlocked for the weapon and then reads
// getPlayerData("weaponRank", <weapon>) (three times for a primary weapon),
// dropping what the level does not give: with a level of 0 a gold MSR spawned
// in its stock finish, though the class in the stats said "gold". So those
// reads are raised as well, and only those: from a read of a weaponSetups path
// until the script reads something that is neither a class nor weaponRank
// (the experience bookkeeping that follows starts with weaponXP). Every other
// weaponRank the script reads is the real one, so a match counts experience
// and level-ups from what was earned, and nothing is written to the stats.
// The result is an integer the reader hands to Scr_AddInt (sub_822B97B0).
#include <ppc_recomp_shared.h>
#include "title.h"
#include "env.h"
#include "mw3_custom_camos.h"
#include "log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#if defined(T_Unlock_RowUnlocked) && defined(T_Unlock_RowUnlockedForController) \
    && defined(T_Profile_IconUnlocked) && defined(T_Profile_TitleUnlocked) && defined(T_Profile_ItemUnlocked) \
    && defined(T_UI_GetPlayerData) && defined(T_StringTable_GetAsset) && defined(T_STR_WeaponRankTable) \
    && defined(T_Scr_GetPlayerData) && defined(T_Scr_GetNumParam) && defined(T_Scr_GetType) \
    && defined(T_Scr_GetString) && defined(T_Scr_AddInt)

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

#define UNLOCK_HOOK(name)                                   \
    GUEST_HOOK(name)                                        \
    {                                                       \
        if (UnlockAll()) { ctx.r3.u64 = 1; return; }        \
        GUEST_ORIG(name)(ctx, base);                        \
    }

UNLOCK_HOOK(T_Unlock_RowUnlocked)
UNLOCK_HOOK(T_Unlock_RowUnlockedForController)
UNLOCK_HOOK(T_Profile_IconUnlocked)
UNLOCK_HOOK(T_Profile_TitleUnlocked)
UNLOCK_HOOK(T_Profile_ItemUnlocked)

namespace
{
    struct WeaponMax { int32_t rank = 0, xp = 0; };

    // The highest level and the most experience mp/weaponRankTable.csv names.
    // A StringTable is {name, columns, rows, cells}, a cell 8 bytes with the
    // text first (sub_82315868). Kept once the table has rows.
    WeaponMax WeaponMaximum(PPCContext& ctx, uint8_t* base)
    {
        static WeaponMax found;
        if (found.rank > 0) return found;

        // The caller's frame is in use above r1; the call gets one of its own
        // below, with the result's word in it.
        const PPCContext saved = ctx;
        const uint32_t frame = saved.r1.u32 - 0x100;
        const uint32_t out = frame + 0x80;
        PPC_STORE_U32(out, 0);
        ctx.r1.u64 = frame;
        ctx.r3.u64 = T_STR_WeaponRankTable;
        ctx.r4.u64 = out;
        GUEST_FUNC(T_StringTable_GetAsset)(ctx, base);
        const uint32_t table = PPC_LOAD_U32(out);
        ctx = saved;
        if (!table) return {};

        const uint32_t columns = PPC_LOAD_U32(table + 4);
        const uint32_t rows = PPC_LOAD_U32(table + 8);
        const uint32_t cells = PPC_LOAD_U32(table + 12);
        if (!cells || columns < 5 || columns > 256 || !rows || rows > 4096) return {};

        WeaponMax most;
        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t column = 0; column < columns; ++column) {
                const uint32_t text = PPC_LOAD_U32(cells + (row * columns + column) * 8);
                if (!text) continue;
                const long value = std::strtol(reinterpret_cast<const char*>(base + text), nullptr, 10);
                if (value <= 0 || value > 0x3FFFFFFF) continue;
                most.xp = std::max(most.xp, int32_t(value));
                if (column == 4) most.rank = std::max(most.rank, int32_t(value));
            }
        }
        if (most.rank > 0) {
            found = most;
            LOGI("unlock: the menus read every weapon at level %d, experience %d (mp/weaponRankTable.csv, %u rows)",
                 most.rank, most.xp, rows);
        }
        return most;
    }
}

// (controller = r3, operands = r4, first operand = r5, result = r6)
GUEST_HOOK(T_UI_GetPlayerData)
{
    const uint32_t operands = ctx.r4.u32;
    const uint32_t first = ctx.r5.u32;
    const uint32_t result = ctx.r6.u32;
    if (!UnlockAll()) {
        GUEST_ORIG(T_UI_GetPlayerData)(ctx, base);
        mw3camo::PlayerDataRead(base, operands, first, result);
        return;
    }

    enum { kNeither, kRank, kExperience } which = kNeither;
    if (first < 10 && first < PPC_LOAD_U32(operands + 0x50) && PPC_LOAD_U32(operands + first * 8) == 2) {
        if (const uint32_t text = PPC_LOAD_U32(operands + first * 8 + 4)) {
            const char* name = reinterpret_cast<const char*>(base + text);
            if (!strcasecmp(name, "weaponRank")) which = kRank;
            else if (!strcasecmp(name, "weaponXP")) which = kExperience;
        }
    }

    GUEST_ORIG(T_UI_GetPlayerData)(ctx, base);
    mw3camo::PlayerDataRead(base, operands, first, result);
    if (which == kNeither || PPC_LOAD_U32(result) != 0) return;

    const WeaponMax most = WeaponMaximum(ctx, base);
    const int32_t value = which == kRank ? most.rank : most.xp;
    if (int32_t(PPC_LOAD_U32(result + 4)) < value) PPC_STORE_U32(result + 4, uint32_t(value));
}

namespace
{
    // The script runs on the server's thread alone.
    bool g_loadoutCheck = false;   // the script is checking a class it has just read
    bool g_raiseResult = false;    // the read in progress is one of that check's weaponRank reads

    // A script call with one argument, on a frame of its own below the hook's.
    uint32_t ScriptCall(PPCContext& ctx, uint8_t* base, PPCFunc* function, uint32_t argument)
    {
        const PPCContext saved = ctx;
        ctx.r1.u64 = saved.r1.u32 - 0x100;
        ctx.r3.u64 = argument;
        function(ctx, base);
        const uint32_t result = ctx.r3.u32;
        ctx = saved;
        return result;
    }

    const char* ScriptString(PPCContext& ctx, uint8_t* base, uint32_t index)
    {
        if (ScriptCall(ctx, base, GUEST_FUNC(T_Scr_GetType), index) != 2) return nullptr;
        const uint32_t text = ScriptCall(ctx, base, GUEST_FUNC(T_Scr_GetString), index);
        return text ? reinterpret_cast<const char*>(base + text) : nullptr;
    }
}

// (the player's entity reference = r3; the path is the script call's arguments)
GUEST_HOOK(T_Scr_GetPlayerData)
{
    if (!UnlockAll()) { GUEST_ORIG(T_Scr_GetPlayerData)(ctx, base); return; }

    const uint32_t count = ScriptCall(ctx, base, GUEST_FUNC(T_Scr_GetNumParam), 0);
    const char* first = count ? ScriptString(ctx, base, 0) : nullptr;
    const bool rank = first && !strcasecmp(first, "weaponRank");
    if (!rank) {
        // customClasses or privateMatchCustomClasses, then the class's number.
        const bool ofClass = first && (!strcasecmp(first, "customClasses") || !strcasecmp(first, "privateMatchCustomClasses"));
        const char* member = ofClass && count > 2 ? ScriptString(ctx, base, 2) : nullptr;
        if (member && !strcasecmp(member, "weaponSetups")) g_loadoutCheck = true;
        else if (!ofClass) g_loadoutCheck = false;
    }

    g_raiseResult = rank && g_loadoutCheck;
    GUEST_ORIG(T_Scr_GetPlayerData)(ctx, base);
    g_raiseResult = false;
}

// (value = r3)
GUEST_HOOK(T_Scr_AddInt)
{
    if (g_raiseResult) {
        g_raiseResult = false;
        const int32_t most = WeaponMaximum(ctx, base).rank;
        if (ctx.r3.s32 < most) {
            static bool said = false;
            if (!said) { said = true; LOGI("unlock: a spawning player's class is checked against weapon level %d", most); }
            ctx.r3.s64 = most;
        }
    }
    GUEST_ORIG(T_Scr_AddInt)(ctx, base);
}

#endif
