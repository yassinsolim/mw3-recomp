// The text of a map pack's Special Ops mission, in Modern Warfare 3.
//
// The disc is of one language and its levels' fastfiles hold their own text.
// A pack is sold to every region, so a mission's text is beside its fastfile
// in one small fastfile a language: english_so_escape_hamburg.ff for
// so_escape_hamburg.ff, and british_, french_, german_, italian_, japanese_,
// polish_, russian_ and spanish_. The title updates that came with the packs
// load the player's language's; the disc's executable asks for the level
// alone, and a pack's mission then showed its objectives and hints as their
// keys (SO_ESCAPE_HAMBURG_OBJ_REGROUP). The packs' Survival maps have no such
// file: their text is the disc's.
//
// So when the title loads a fastfile that a package holds the English text
// for, that one is put before it in the same list, with the same flags: it
// is loaded and let go with the level. English, because the disc this port
// plays is (the same choice as the Advanced Warfare update's text,
// aw_update_language.cpp).
#include <ppc_recomp_shared.h>
#include "title.h"
#include "guest.h"
#include "log.h"
#include "kernel/kernel.h"

#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#if defined(MW3_TITLE) && defined(T_DB_LoadXAssets)

namespace
{
    constexpr uint32_t kZoneInfo = 12;      // XZoneInfo { name, allocFlags, freeFlags }
    constexpr uint32_t kListed = 16;        // the title's longest list is three
    constexpr char kLanguage[] = "english_";

    std::string Lower(std::string text)
    {
        for (char& c : text) c = char(std::tolower(uint8_t(c)));
        return text;
    }

    // The guest's copy of a text fastfile's name, 0 when no package holds
    // one for `zone`. Kept: the title keeps a loaded fastfile's name by its
    // pointer, and there are as many as the player has missions.
    uint32_t TextZone(const std::string& zone)
    {
        static std::mutex lock;
        static std::map<std::string, uint32_t> names;
        std::lock_guard held(lock);
        const auto known = names.find(zone);
        if (known != names.end()) return known->second;
        const std::string wanted = kLanguage + zone;
        uint32_t at = 0;
        for (const kernel::ContentFile& file : kernel::DownloadedContentFiles())
            if (!at && Lower(file.name) == wanted + ".ff")
            {
                at = kernel::AllocateGuest(uint32_t(wanted.size() + 1), 4);
                if (at) std::memcpy(GuestPtr<char>(at), wanted.c_str(), wanted.size() + 1);
            }
        names[zone] = at;
        return at;
    }
}

GUEST_HOOK(T_DB_LoadXAssets)
{
    const uint32_t zones = ctx.r3.u32, count = ctx.r4.u32;
    // One list at a time is being loaded: the title waits for the last.
    static const uint32_t list = kernel::AllocateGuest(kListed * kZoneInfo, 4);
    uint32_t filled = 0;
    bool added = false;
    for (uint32_t i = 0; list && i < count && filled + 2 <= kListed; i++)
    {
        const uint32_t from = zones + i * kZoneInfo;
        const uint32_t name = PPC_LOAD_U32(from);
        if (const uint32_t text = name ? TextZone(Lower(GuestPtr<char>(name))) : 0)
        {
            const uint32_t to = list + filled++ * kZoneInfo;
            PPC_STORE_U32(to, text);
            PPC_STORE_U32(to + 4, PPC_LOAD_U32(from + 4));
            // What the level's entry frees is freed once, by the level's.
            PPC_STORE_U32(to + 8, 0);
            added = true;
            LOGI("pack text: %s is loaded with %s", GuestPtr<char>(text), GuestPtr<char>(name));
        }
        std::memcpy(GuestPtr<uint8_t>(list + filled++ * kZoneInfo), GuestPtr<uint8_t>(from), kZoneInfo);
    }
    if (added)
    {
        ctx.r3.u64 = list;
        ctx.r4.u64 = filled;
    }
    GUEST_ORIG(T_DB_LoadXAssets)(ctx, base);
}

#endif
