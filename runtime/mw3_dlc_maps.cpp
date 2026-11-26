// The maps of a player's own downloadable content in Modern Warfare 3's
// multiplayer menus.
//
// The disc's executable loads a pack's map: the fastfiles are of its version
// and it finds them on the pack's drive (kernel/content.cpp). What it has not
// got is the maps' names. Its menus are ready for packs: the arena table,
// which UI_LoadArenas fills from mp/basemaps.arena (a raw file of the disc's
// patch_mp.ff), begins with a row that is no map, "bonus_map", which the
// lobby's MAPS list shows as BONUS MAPS once any arena names a pack the
// player has (its `mappack`, which the title turns into the flag 2 << n and
// looks up among the packs it mounted). That row opens settings_bonus_map,
// which lists the maps of the player's packs. The disc's arena file names
// its own sixteen maps and no pack's: those came with title updates, which
// a player's packages do not hold.
//
// So for each mp_<name>.ff among the files of the packages the title was
// given, an arena is parsed after the title's own, with the title's parser:
// the map, its name, the pack the title takes its drive for, and the game
// types of the disc's first map. The name is the one the game gave the map,
// from the table below, since no file of a pack has it; a map the table
// does not know is listed by its file's name. The description names the
// package it is in, as the console shows it, and the picture is the one the
// title's BONUS MAPS row has: a pack's own preview came with the updates
// too. The menus look a name up as a translation ("@" and the longname) and
// show the text itself when there is none.
//
// The arena also says which soldiers the two teams are and which ghillie
// suit the snipers wear (allieschar, axischar, environment), which the game
// type's script asks for when the level starts; with none it takes the
// S.A.S. and the henchmen. A map's fastfile holds the models of its own two
// teams and no others, so on most of the packs' maps the players then had
// the stand-in model, and the first one a player looked at ended the match
// ("Couldn't find [j_head] on player"). What the update's arena said is
// what the map's fastfile holds: when a pack's map is loading, the teams
// whose soldiers the database now has are found (each by one model its
// scripts set), and the suit by the ghillie models, and the map's arena is
// given them.
//
// Nothing is listed that the player's packages do not hold, and the map's
// own fastfile is what loads: a player without the pack cannot start or join
// the map.
//
// The packs' list has two mistakes of the disc's, which no map could show
// before a pack existed. The row the cursor belongs on is answered as the
// map's index among all maps, the disc's included, where the list has the
// packs' alone: the cursor sat on a row sixteen below the map chosen, or on
// none, and moving it chose nothing. The row is counted here. And the list
// is nineteen rows high where the menu has room for seventeen above its
// footer, which the disc's own seventeen rows just fill, and the menu draws
// the cursor as though the list never scrolled: the list is made high
// enough for every map, and the footer's two lines beside it are moved to
// under the map's picture.
#include <ppc_recomp_shared.h>
#include "mw3_dlc_maps.h"
#include "title.h"
#include "log.h"
#include "kernel/kernel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#if defined(MW3_TITLE) && defined(T_UI_LoadArenas)

namespace
{
    constexpr uint32_t kRoom = 64;              // the info strings the table holds
    constexpr uint32_t kBufferSize = 0x18000;   // UI_LoadArenas' own buffer
    constexpr uint32_t kCount = 256;            // after the first info string

    // The names the game gave the maps of its packs, by fastfile.
    constexpr struct { const char* map; const char* name; } kNames[] = {
        { "mp_italy", "Piazza" },             { "mp_park", "Liberation" },
        { "mp_overwatch", "Overwatch" },      { "mp_morningwood", "Black Box" },
        { "mp_meteora", "Sanctuary" },        { "mp_cement", "Foundation" },
        { "mp_qadeem", "Oasis" },             { "mp_hillside_ss", "Getaway" },
        { "mp_restrepo_ss", "Lookout" },      { "mp_courtyard_ss", "Erosion" },
        { "mp_aground_ss", "Aground" },       { "mp_terminal_cls", "Terminal" },
        { "mp_burn_ss", "U-Turn" },           { "mp_six_ss", "Vortex" },
        { "mp_crosswalk_ss", "Intersection" }, { "mp_shipbreaker", "Decommission" },
        { "mp_roughneck", "Offshore" },       { "mp_moab", "Gulch" },
        { "mp_boardwalk", "Boardwalk" },      { "mp_nola", "Parish" },
    };

    std::string Lower(std::string text)
    {
        for (char& c : text) c = char(std::tolower(uint8_t(c)));
        return text;
    }

    // What an info string has for `key`: "\key\value\key\value".
    std::string ValueOf(const std::string& info, const std::string& key)
    {
        const size_t at = info.find("\\" + key + "\\");
        if (at == std::string::npos) return {};
        const size_t from = at + key.size() + 2;
        return info.substr(from, info.find('\\', from) - from);
    }

    // The arena's text may not end a quoted value or an info string.
    std::string Plain(std::string text)
    {
        for (char& c : text)
            if (c == '"' || c == '\\' || c == '{' || c == '}' || uint8_t(c) < 0x20) c = ' ';
        return text;
    }

    // menuDef_t and itemDef_s as mw3_custom_camos.cpp has them: an item's
    // name, then its rect {x, y, w, h} and the rect it was given.
    constexpr uint32_t kMenuItemCount = 0xB4, kMenuItems = 0xB8;
    constexpr uint32_t kItemName = 0x00, kItemRect = 0x04, kItemGivenRect = 0x18;
    // The packs' list, with and without the map rotation (a private match's is with).
    constexpr const char* kListMenus[] = { "settings_bonus_map", "settings_bonus_map_rotation" };
    constexpr char kListItem[] = "map_selection";
    // Rows of twenty units, and two units the title takes off the height.
    constexpr float kListHeight = 2 + 24 * 20.0f;
    // "Select Map" and "Enable Map Rotation", which the list's last rows
    // would be drawn over: the items of the list's column (left of the
    // divider) from this height down, and how far right they go, to under
    // the map's picture.
    constexpr float kHintTop = 360, kDivider = 237.5f, kHintMove = 300;

    float Float(uint8_t* base, uint32_t at)
    {
        const uint32_t bits = PPC_LOAD_U32(at);
        float value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }

    void SetFloat(uint8_t* base, uint32_t at, float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        PPC_STORE_U32(at, bits);
    }
}

void mw3maps::MenuAdded(uint8_t* base, uint32_t menu, const char* name)
{
    if (std::none_of(std::begin(kListMenus), std::end(kListMenus), [name](const char* one) { return !std::strcmp(name, one); }))
        return;
    const uint32_t count = PPC_LOAD_U32(menu + kMenuItemCount), items = PPC_LOAD_U32(menu + kMenuItems);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t item = PPC_LOAD_U32(items + i * 4);
        if (!item) continue;
        const uint32_t itemName = PPC_LOAD_U32(item + kItemName);
        const bool list = itemName && !std::strcmp(reinterpret_cast<const char*>(base + itemName), kListItem);
        for (const uint32_t rect : { kItemRect, kItemGivenRect })
        {
            if (list) SetFloat(base, item + rect + 12, kListHeight);
            // A menu is added more than once: what was moved is past the divider.
            const float x = Float(base, item + rect), y = Float(base, item + rect + 4);
            if (!list && y >= kHintTop && x >= 0 && x < kDivider) SetFloat(base, item + rect, x + kHintMove);
        }
    }
}

namespace
{
    // A team's soldiers, as an arena names them, and a model their scripts
    // set (character/mp_character_<...>_assault).
    struct Soldiers { const char* name; const char* model; };
    constexpr Soldiers kAllies[] = {
        { "delta_multicam", "mp_body_delta_elite_assault_aa" }, { "sas_urban", "mp_body_sas_urban_assault" },
        { "gign_paris", "mp_body_gign_paris_assault" },         { "pmc_africa", "mp_body_pmc_africa_assault_a" },
    };
    constexpr Soldiers kAxis[] = {
        { "opforce_air", "mp_body_russian_military_assault_a_airborne" },
        { "opforce_snow", "mp_body_russian_military_assault_a_arctic" },
        { "opforce_woodland", "mp_body_russian_military_assault_a_woodland" },
        { "opforce_urban", "mp_body_russian_military_assault_a" },
        { "opforce_henchmen", "mp_body_henchmen_assault_a" },
        { "opforce_africa", "mp_body_africa_militia_assault_a" },
    };
    // The ghillie suits (maps/mp/gametypes/_teams, setghilliemodels): the
    // environment, and the model the other team's sniper wears in it.
    constexpr Soldiers kSuits[] = {
        { "arctic", "mp_body_opforce_ghillie_arctic_sniper" }, { "desert", "mp_body_opforce_ghillie_desert_sniper" },
        { "urban", "mp_body_opforce_ghillie_urban_sniper" },   { "forest", "mp_body_opforce_ghillie_woodland_sniper" },
    };
    // The militia's own suit, with the allies' of a place: "<place>_militia".
    constexpr char kMilitiaSuit[] = "mp_body_opforce_ghillie_africa_militia_sniper";
    constexpr Soldiers kMilitiaPlaces[] = {
        { "urban", "mp_body_ally_ghillie_urban_sniper" }, { "forest", "mp_body_ally_ghillie_woodland_sniper" },
        { "desert", "mp_body_ally_ghillie_desert_sniper" },
    };
    constexpr uint32_t kModel = 4;   // the database's type of an xmodel

    // What was found for each map: its arena's three keys, kept for when
    // the arenas are next read.
    using Keys = std::vector<std::pair<std::string, std::string>>;
    std::map<std::string, Keys> g_found;

    // A map of the menus' table (title.h, T_UI_MAPS): the pack it is of, and
    // every key of its arena with its value, up to 32 of them.
    constexpr uint32_t kMapPack = 0xA74, kMapKeyCount = 0xA70, kMapKeys = 0x70, kMapValues = 0x270;
    constexpr uint32_t kMapKeySize = 16, kMapValueSize = 64, kMapMostKeys = 32;

    bool Has(PPCContext& ctx, uint8_t* base, const char* model)
    {
        static const uint32_t name = kernel::AllocateGuest(128, 16);
        if (!name) return false;
        std::snprintf(reinterpret_cast<char*>(base + name), 128, "%s", model);
        const PPCContext saved = ctx;
        ctx.r1.u64 = saved.r1.u32 - 0x100;
        ctx.r3.u64 = kModel;
        ctx.r4.u64 = name;
        GUEST_FUNC(T_DB_FindXAssetEntry)(ctx, base);
        bool has = ctx.r3.u32 != 0;
        if (has)
        {
            ctx.r3.u64 = kModel;
            ctx.r4.u64 = name;
            GUEST_FUNC(T_DB_IsXAssetDefault)(ctx, base);
            has = (ctx.r3.u32 & 0xFF) == 0;
        }
        ctx = saved;
        return has;
    }

    template <size_t N> const char* FirstHad(PPCContext& ctx, uint8_t* base, const Soldiers (&among)[N])
    {
        for (const Soldiers& one : among)
            if (Has(ctx, base, one.model)) return one.name;
        return nullptr;
    }
}

void mw3maps::LevelLoading(PPCContext& ctx, uint8_t* base)
{
    const PPCContext saved = ctx;
    ctx.r1.u64 = saved.r1.u32 - 0x100;
    ctx.r3.u64 = T_DATA_MapNameDvarName;
    ctx.r4.u64 = T_DATA_EmptyString;
    ctx.r5.u64 = T_MAPNAME_DVAR_FLAGS;
    ctx.r6.u64 = T_DATA_MapNameDvarText;
    GUEST_FUNC(T_Dvar_RegisterString)(ctx, base);
    const uint32_t dvar = ctx.r3.u32;
    ctx = saved;
    const uint32_t text = dvar ? PPC_LOAD_U32(dvar + 8) : 0;
    if (!text) return;
    const std::string map = Lower(reinterpret_cast<const char*>(base + text));
    const uint32_t count = std::min<uint32_t>(PPC_LOAD_U32(T_DATA_UiInfo + T_UI_MAP_COUNT), 128);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t entry = T_DATA_UiInfo + T_UI_MAPS + i * T_UI_MAP_SIZE;
        if (Lower(reinterpret_cast<const char*>(base + entry + T_UI_MAP_NAME)) != map) continue;
        // The disc's own maps say theirs, and so does a pack's whose arena
        // was read after its soldiers were found.
        uint32_t keys = PPC_LOAD_U32(entry + kMapKeyCount);
        if (!PPC_LOAD_U32(entry + kMapPack)) return;
        for (uint32_t k = 0; k < keys; k++)
            if (!std::strcmp(reinterpret_cast<const char*>(base + entry + kMapKeys + k * kMapKeySize), "allieschar")) return;

        Keys& found = g_found[map];
        if (found.empty())
        {
            const char* allies = FirstHad(ctx, base, kAllies);
            const char* axis = FirstHad(ctx, base, kAxis);
            if (!allies || !axis)
            {
                LOGW("maps: %s: its fastfile has the soldiers of no team this game knows (allies %s, axis %s); the title's own are used",
                     map.c_str(), allies ? allies : "none", axis ? axis : "none");
                return;
            }
            found = { { "allieschar", allies }, { "axischar", axis } };
            if (Has(ctx, base, kMilitiaSuit))
            {
                if (const char* place = FirstHad(ctx, base, kMilitiaPlaces))
                    found.emplace_back("environment", std::string(place) + "_militia");
            }
            else if (const char* place = FirstHad(ctx, base, kSuits)) found.emplace_back("environment", place);
            LOGI("maps: %s is played by %s and %s%s%s, the soldiers its fastfile holds", map.c_str(), allies, axis,
                 found.size() > 2 ? ", ghillie suits of " : "", found.size() > 2 ? found[2].second.c_str() : "");
        }
        for (const auto& [key, value] : found)
        {
            if (keys >= kMapMostKeys) break;
            std::snprintf(reinterpret_cast<char*>(base + entry + kMapKeys + keys * kMapKeySize), kMapKeySize, "%s", key.c_str());
            std::snprintf(reinterpret_cast<char*>(base + entry + kMapValues + keys * kMapValueSize), kMapValueSize, "%s", value.c_str());
            keys++;
        }
        PPC_STORE_U32(entry + kMapKeyCount, keys);
        return;
    }
}

GUEST_HOOK(T_UI_LoadArenas)
{
    GUEST_ORIG(T_UI_LoadArenas)(ctx, base);

    const uint32_t count = PPC_LOAD_U32(T_DATA_UI_ArenaInfos + kCount);
    std::vector<std::string> have;
    std::string gametypes, picture;
    for (uint32_t i = 0; i < count && i < kRoom; i++)
    {
        const std::string info = reinterpret_cast<const char*>(base + PPC_LOAD_U32(T_DATA_UI_ArenaInfos + i * 4));
        have.push_back(Lower(ValueOf(info, "map")));
        if (have.back() == "bonus_map") picture = ValueOf(info, "mapimage");
        if (gametypes.empty()) gametypes = ValueOf(info, "gametype");
    }

    struct Map { std::string map, name, package; uint32_t pack; };
    std::vector<Map> maps;
    for (const kernel::ContentFile& file : kernel::DownloadedContentFiles())
    {
        const std::string name = Lower(file.name);
        const bool map = name.size() > 6 && name.compare(0, 3, "mp_") == 0 && name.compare(name.size() - 3, 3, ".ff") == 0 &&
                         name.find("_load.ff") == std::string::npos;
        if (!map) continue;
        const std::string stem = name.substr(0, name.size() - 3);
        if (std::find(have.begin(), have.end(), stem) != have.end()) continue;
        have.push_back(stem);
        std::string longname = stem;
        for (const auto& known : kNames)
            if (stem == known.map) longname = known.name;
        maps.push_back({ stem, longname, file.package, file.pack });
    }
    std::sort(maps.begin(), maps.end(), [](const Map& a, const Map& b) { return a.name < b.name; });

    std::string text;
    for (const Map& map : maps)
    {
        std::string soldiers;
        if (const auto found = g_found.find(map.map); found != g_found.end())
            for (const auto& [key, value] : found->second) soldiers += key + " \"" + value + "\"\n";
        const std::string arena = "{\nmap \"" + map.map + "\"\nlongname \"" + Plain(map.name) + "\"\ngametype \"" +
                                  Plain(gametypes) + "\"\ndescription \"" + Plain(map.package) + "\"\nmapimage \"" +
                                  Plain(picture) + "\"\n" + soldiers + "mappack \"" + std::to_string(map.pack) + "\"\n}\n";
        if (text.size() + arena.size() >= kBufferSize) break;
        text += arena;
    }
    if (text.empty()) return;
    if (count >= kRoom)
    {
        LOGW("maps: the map list is full; the %zu maps of the player's packages are left out", maps.size());
        return;
    }

    const PPCContext saved = ctx;
    // A frame of its own below the caller's, for the buffer's handle.
    ctx.r1.u64 = saved.r1.u32 - 128;
    const uint32_t handle = ctx.r1.u32 + 80;
    ctx.r3.u64 = handle;
    ctx.r4.u64 = kBufferSize;
    GUEST_FUNC(T_LargeLocal_Begin)(ctx, base);
    ctx.r3.u64 = handle;
    GUEST_FUNC(T_LargeLocal_GetBuf)(ctx, base);
    const uint32_t buffer = ctx.r3.u32;
    std::memcpy(base + buffer, text.c_str(), text.size() + 1);
    ctx.r3.u64 = buffer;
    ctx.r4.u64 = kRoom - count;
    ctx.r5.u64 = T_DATA_UI_ArenaInfos + count * 4;
    GUEST_FUNC(T_UI_ParseInfos)(ctx, base);
    const uint32_t added = ctx.r3.u32;
    PPC_STORE_U32(T_DATA_UI_ArenaInfos + kCount, count + added);
    ctx.r3.u64 = handle;
    GUEST_FUNC(T_LargeLocal_End)(ctx, base);
    ctx = saved;
    LOGI("maps: %u of the %zu maps of the player's packages are in the BONUS MAPS list", added, maps.size());
}

GUEST_HOOK(T_UI_MapRowOfName)
{
    GUEST_ORIG(T_UI_MapRowOfName)(ctx, base);
    const uint32_t index = ctx.r3.u32;
    const uint32_t dvar = PPC_LOAD_U32(T_DATA_UI_BonusMapsDvar);
    if (!index || !dvar || !PPC_LOAD_U8(dvar + 8)) return;

    const PPCContext saved = ctx;
    GUEST_FUNC(T_UI_BonusMapCount)(ctx, base);
    const uint32_t rows = ctx.r3.u32;
    uint32_t row = 0;
    for (uint32_t i = 0; i < rows && !row; i++)
    {
        ctx.r3.u64 = i;
        GUEST_FUNC(T_UI_MapOfRow)(ctx, base);
        if (ctx.r3.u32 == index) row = i;
    }
    ctx = saved;
    ctx.r3.u64 = row;
}

#endif
