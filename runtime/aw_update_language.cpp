// A title update of another pressing than the disc, in Advanced Warfare's
// multiplayer as the update makes it (AW_TITLE_TU17).
//
// An update is made for one pressing of the disc and carries the localised
// patch fastfiles of that pressing's language alone: a Latin American disc's
// update has spanishna\sna_patch_*.ff and nothing of English's. The game disc
// this port plays is English, and its executable is the same in every
// pressing, so such an update's patch applies to it (docs/dlc.md) -- but the
// title then asks for english\eng_patch_*.ff, finds the disc's of the day it
// was pressed, and has none of what the update added. That is not only text:
// this game keeps its string tables in the localised fastfiles, among them
// mp/ingamestore/s1_mdlcEntitlements.csv, the list of every pack the title
// knows a package by. Without it no package is identified, and the title
// leaves with DLC_DLCREMOVED as it starts.
//
// So the update's own localised fastfile is loaded as well, under its own
// name (it is signed under that name, and nothing here loosens the check):
// before the title loads eng_<zone> (eng_patch_<zone>, and the few whole
// fastfiles an update brings: the zombies'), the update's <letters>_<zone> is
// loaded by the title's own function. The first fastfile to bring an asset keeps it,
// which is what gives the update's tables precedence over the disc's older
// ones -- and would give its text precedence over English. So the text is
// left out as the fastfile's assets are added, by the asset's type: a
// localised string of the update's language is not added (the title has room
// for 13,360 strings, and the update's 12,957 beside English's are more), its
// text is kept here, and the English fastfiles that follow bring their own.
// What only the update has (the packs' descriptions, the menus' new labels:
// 1,412 strings with the Latin American update) has no English anywhere in
// the player's files, and is answered with the update's text, in its
// language. The maps' and the modes' names are the same in both.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "guest.h"
#include "log.h"
#include "env.h"
#include "diagnostics.h"
#include "kernel/kernel.h"

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#if defined(AW_TITLE_TU17)

namespace
{
    std::mutex g_lock;
    bool g_foreign = false;                 // the update's own localised fastfile is being loaded
    unsigned g_taken = 0, g_leftOut = 0;
    std::map<std::string, std::string> g_updateText;    // the strings left out: key -> the update's text
    std::vector<std::string> g_pending;                 // those left out since the last count
    std::map<std::string, uint32_t> g_answers;          // a key English has not -> the entry answered (0: none)
    unsigned g_onlyInUpdate = 0, g_shown = 0;
    std::set<std::string> g_lists;                      // the update's lists of network names taken

    bool Trace()
    {
        static const bool on = diag::Flag("MW2_TRACE_UPDATE_TEXT");
        return on;
    }

    uint32_t GuestText(const std::string& text)
    {
        const uint32_t at = kernel::AllocateGuest(uint32_t(text.size() + 1), 4);
        if (at) std::memcpy(GuestPtr<char>(at), text.c_str(), text.size() + 1);
        return at;
    }

    // The title's own search, past the hook below.
    uint32_t Find(PPCContext& ctx, uint8_t* base, uint32_t type, uint32_t name);

    // Once the English fastfiles of a group are in: how many of the strings
    // left out have no English.
    void CountPending(PPCContext& ctx, uint8_t* base)
    {
        std::vector<std::string> pending;
        {
            std::lock_guard held(g_lock);
            pending.swap(g_pending);
        }
        if (pending.empty()) return;
        unsigned only = 0;
        for (const std::string& key : pending)
        {
            const uint32_t asked = GuestText(key);
            const bool english = Find(ctx, base, T_ASSET_LOCALIZE, asked) != 0;
            kernel::FreeGuest(asked);
            std::lock_guard held(g_lock);
            if (english) g_updateText.erase(key);
            else
            {
                only++;
                if (Trace()) LOGI("update text: only in the update: %s = %s", key.c_str(), g_updateText[key].c_str());
            }
        }
        std::lock_guard held(g_lock);
        g_onlyInUpdate += only;
        LOGI("update text: %u of %zu strings left out have no English in the player's files (%u so far)", only,
             pending.size(), g_onlyInUpdate);
    }
}

// (name, flags): loads one fastfile.
GUEST_HOOK(T_DB_LoadZone)
{
    const uint32_t name = ctx.r3.u32, flags = ctx.r4.u32;
    const std::string zone = name ? GuestPtr<char>(name) : "";
    const std::string& letters = kernel::UpdateLanguage();
    // A group is patch_<zone>, eng_patch_<zone>, eng_<zone>, <zone>: at the
    // next name that is not English's, the group's English is in.
    if (!letters.empty() && zone.rfind("eng_", 0) != 0) CountPending(ctx, base);
    if (!letters.empty() && zone.rfind("eng_", 0) == 0)
    {
        const std::string own = letters + zone.substr(3);
        if (kernel::UpdateHasZone(own))
        {
            const uint32_t text = GuestText(own);
            {
                std::lock_guard held(g_lock);
                g_foreign = true; g_taken = g_leftOut = 0;
            }
            ctx.r3.u64 = text; ctx.r4.u64 = flags;
            GUEST_ORIG(T_DB_LoadZone)(ctx, base);
            std::lock_guard held(g_lock);
            g_foreign = false;
            LOGI("update text: %s before %s: %u assets taken (tables and the like), %u strings left out for English's",
                 own.c_str(), zone.c_str(), g_taken, g_leftOut);
        }
    }
    ctx.r3.u64 = name; ctx.r4.u64 = flags;
    GUEST_ORIG(T_DB_LoadZone)(ctx, base);
}

// (type, header*): adds one asset of the fastfile being loaded, and returns
// its record.
GUEST_HOOK(T_DB_LinkAsset)
{
    const uint32_t type = ctx.r3.u32, header = ctx.r4.u32 ? uint32_t(*GuestPtr<be32>(ctx.r4.u32)) : 0;
    {
        std::lock_guard held(g_lock);
        if (g_foreign && header && type == T_ASSET_LOCALIZE)
        {
            // { value, name }. Nothing refers to a string but by its name, so
            // the loader is told its own header is the one in use: a record
            // of this file's with nothing else in it, read by the caller's
            // next instruction (one thread loads a fastfile).
            const uint32_t value = *GuestPtr<be32>(header), key = *GuestPtr<be32>(header + 4);
            if (value && key)
            {
                static const uint32_t record = kernel::AllocateGuest(16, 4);
                g_updateText[GuestPtr<char>(key)] = GuestPtr<char>(value);
                g_pending.emplace_back(GuestPtr<char>(key));
                g_leftOut++;
                *GuestPtr<be32>(record + T_ASSET_RECORD_HEADER) = header;
                ctx.r3.u64 = record;
                return;
            }
        }
        if (header && type == T_ASSET_NETCONSTSTRINGS)
        {
            // { name, ... }: the names a fastfile's strings go over the
            // network by, "ncs_<kind>_<letters>_<zone>" for a localised
            // fastfile's (loc its strings, hnt its hints). The title has room
            // for one language's (690 strings, where the update's 567 and
            // the disc's 546 of one fastfile are more; 255 hints), and a
            // match then ends in EXE_TRANSMITERROR. So where the update's
            // list was taken English's of the same fastfile is not.
            const uint32_t name = *GuestPtr<be32>(header);
            const std::string list = name ? GuestPtr<char>(name) : "";
            const bool english = list.size() > 12 && list.rfind("ncs_", 0) == 0 && list.compare(7, 5, "_eng_") == 0;
            if (g_foreign) g_lists.insert(list);
            else if (english && g_lists.count(list.substr(0, 8) + kernel::UpdateLanguage() + list.substr(11)))
            {
                static const uint32_t record = kernel::AllocateGuest(16, 4);
                *GuestPtr<be32>(record + T_ASSET_RECORD_HEADER) = header;
                ctx.r3.u64 = record;
                LOGI("update text: %s is left out for the update's list of the same fastfile", list.c_str());
                return;
            }
        }
        if (g_foreign) g_taken++;
    }
    GUEST_ORIG(T_DB_LinkAsset)(ctx, base);
}

// (type, name, make a default). A string English has not: the update's own
// text.
GUEST_HOOK(T_DB_FindAsset)
{
    const uint32_t type = ctx.r3.u32, name = ctx.r4.u32;
    GUEST_ORIG(T_DB_FindAsset)(ctx, base);
    if (ctx.r3.u32 || !name || type != T_ASSET_LOCALIZE) return;
    const std::string key = GuestPtr<char>(name);
    std::lock_guard held(g_lock);
    if (const auto known = g_answers.find(key); known != g_answers.end()) { ctx.r3.u64 = known->second; return; }
    const auto update = g_updateText.find(key);
    if (update == g_updateText.end()) return;
    // { value, name }, as the title's own entries.
    const uint32_t answer = kernel::AllocateGuest(8, 4);
    *GuestPtr<be32>(answer) = GuestText(update->second);
    *GuestPtr<be32>(answer + 4) = GuestText(key);
    g_answers[key] = answer;
    g_shown++;
    if (Trace()) LOGI("update text: %s has no English and is shown in the update's language", key.c_str());
    else if ((g_shown & (g_shown - 1)) == 0)
        LOGI("update text: %u strings with no English have been shown in the update's language", g_shown);
    ctx.r3.u64 = answer;
}

namespace
{
    uint32_t Find(PPCContext& ctx, uint8_t* base, uint32_t type, uint32_t name)
    {
        ctx.r3.u64 = type; ctx.r4.u64 = name; ctx.r5.u64 = 0;
        GUEST_ORIG(T_DB_FindAsset)(ctx, base);
        return ctx.r3.u32;
    }
}
#endif
