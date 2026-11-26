// Custom camouflages in Modern Warfare 3's Create a Class: the camouflages of
// the packs in the game folder's `camos` (camo_packs.h) are listed below Gold,
// each with its own name, description and preview, chosen per class and per
// weapon, and drawn on the weapon in a match. No camouflage of the game's is
// replaced, and nothing but Gold is ever written to the stats.
//
// THE LIST. The camouflage menus (cac_weapon_camo_primary and ..._secondary)
// do not read mp/camoTable.csv by row: each camouflage is six items written
// out in the menu, a button named WEAPON_CAMO_LIST_<n> and five that follow
// it (two lock icons, the label in its two colours and the "new" marker),
// fourteen such rows, with the camouflage's name ("gold") and the row's
// number constants of their expressions. Rows 12 and 13 (marine, winter)
// show only to a player the stats say has them. The rows are a button group
// that scrolls already: every row's height is
// `34.67 + 20 * row - 20 * localVarInt("ui_buttonNavGroupOffset")`, and a
// button's onFocus moves that offset to keep itself within fourteen rows.
// With fourteen rows it never moved, so only the "new" marker tests whether
// its row is inside the window.
//
// So the list is extended in the menu itself, in guest memory, when the
// menus are added to the context (UI_AddMenuList): Gold's six items are copied
// once per camouflage, with the row number, the name and what follows from
// them changed in the copies' expressions and handlers, and marine and winter
// are copied to the rows after those. Every row's items gain the window test
// the marker has, so that rows scrolled out are not drawn over the title or
// the footer: the five by their visibility, and the button, which must stay
// visible to be given the focus that scrolls the list to it, by its height.
// The menu's item list is replaced by one with the copies in it. What a copy
// shares with Gold's row (most of its handlers) is not copied.
//
// THE CHOICE. A copy is named cc<nnn> in its expressions. mp/camoTable.csv has
// no such row, so the menu's tableLookup (StringTable_Lookup) is answered for
// these names: the pack's name for the camouflage, and Gold's material for
// the preview and the banner. The button's action still writes "gold" to the
// class (the stats' enum has nothing else to hold), and the hook on the
// menu's setPlayerData notes which copy's script is running, by the script's
// address, and records `<class>.weaponSetups.<n>` -> `<pack>/<camo>` in
// saves/custom_camos.txt. The menu's getPlayerData then answers cc<nnn> for a
// class whose camouflage is stored as Gold and recorded there, so the list
// marks the right row and the class summary names it; a script that writes a
// value it read that way (copying a class) is given "gold" for the stats and
// the record is copied with it. Choosing any of the game's camouflages, or
// another weapon, removes the record. Only a write to a slot the stats hold
// Gold in, or a write of Gold, is such a choice: when the player signs in the
// menus look every class over and write "none" throughout one that has no
// weapon, and on a phone that comes before the stats have arrived, when every
// class reads so. That took the record of every class at each start, and a
// camouflage chosen in one run was Gold in the next.
//
// THE PICTURE. Gold on a weapon is one image (docs/textures.md), as are the
// menu's preview and banner for it. They are made switchable
// (gpu::replace::Switchable): the preview shows the row that has focus (the
// menu's own lookup of that row's material, StringTable_Lookup), the banner
// the class being looked at, and the weapon image the camouflage of the
// class the player chose in the match (the changeclass menu's
// scriptmenuresponse "custom<n>"), its primary weapon's before its
// secondary's. One image means one camouflage at a time on this device:
// every Gold weapon in view, other players' included, is drawn with it, and
// other players see Gold. Without a choice Gold is Gold. A camouflage with a
// picture a weapon (a pack made from whole-weapon skins) is drawn by the
// weapon the class has in that slot, noted as the menus read it; a still
// one the pack or the player animates is given its frames by the renderer
// (gpu::replace::Effect).
#include <ppc_recomp_shared.h>
#include "mw3_custom_camos.h"
#include "mw3_dlc_maps.h"
#include "title.h"
#include "camo_packs.h"
#include "env.h"
#include "gpu/texture_replace.h"
#include "kernel/kernel.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <strings.h>
#include <vector>

#if defined(T_UI_AddMenuList) && defined(T_UI_SetPlayerData) && defined(T_UI_ScriptMenuResponse) \
    && defined(T_UI_FindLocalVar) && defined(T_StringTable_Lookup) && defined(T_UI_GetPlayerData)

// The menus' reader of the stats, hooked in mw3_unlock_all.cpp and called
// here as the title wrote it (StoredCamo).
GUEST_ORIGINAL(T_UI_GetPlayerData);

namespace
{
    namespace fs = std::filesystem;

    // ---- the title's structures, as far as they are read here ----
    // UiContext
    constexpr uint32_t kContextMenus = 52, kContextMenuCount = 2612, kContextLocalVars = 2684;
    constexpr uint32_t kMostMenus = 640;
    // menuDef_t: {data, window (0xB0 bytes, its name first), itemCount, items}
    constexpr uint32_t kMenuName = 0x04, kMenuItemCount = 0xB4, kMenuItems = 0xB8;
    // itemDef_s. The window is first: name, rect {x, y, w, h}, the rect it was
    // given, and from 0x48 the dynamic flags of the four local clients (bit 1:
    // has focus). Then nine handler sets, the type's data, the expressions
    // that drive rect and colour, and four more for visibility, disabling,
    // text and material.
    constexpr uint32_t kItemSize = 0x1D4;
    constexpr uint32_t kItemName = 0x00, kItemY = 0x08, kItemGivenY = 0x1C, kItemFlags = 0x48;
    constexpr uint32_t kItemHandlers = 0x138, kItemHandlerCount = 9;
    constexpr uint32_t kItemTypeData = 0x18C, kItemTypeDataSize = 0x20;
    constexpr uint32_t kItemFloatCount = 0x190, kItemFloats = 0x194;
    constexpr uint32_t kItemVisible = 0x198, kItemDisabled = 0x19C, kItemText = 0x1A0, kItemMaterial = 0x1A4;
    // Statement_s: {entry count, entries, supporting data}, then what its
    // last evaluation left for each local client: 0x50 bytes. An entry is
    // {0, operator} or {1, type, value}, the type 0 an integer and 2 a string.
    constexpr uint32_t kStatementSize = 0x50, kEntrySize = 12;
    // MenuEventHandlerSet {count, handlers}; MenuEventHandler {data, type}:
    // 0 a script's text, 1 {set, condition}, 2 an else's set, 3..6
    // {variable name, expression} for a local bool, int, float or string.
    enum : uint8_t { kScript = 0, kIf = 1, kElse = 2, kSetLocalFirst = 3, kSetLocalLast = 6 };
    // Operators: below 23 they are punctuation, from 23 on functions, each of
    // which opens a parenthesis that operator 1 closes.
    enum : uint32_t {
        kClose = 1, kTimes = 2, kPlus = 5, kMinus = 6, kNot = 7, kLess = 8, kAtLeast = 11, kNotEqual = 13,
        kAnd = 14, kOpen = 16, kFirstFunction = 23, kLocalVarInt = 79,
    };

    constexpr uint32_t kRowItems = 6;      // a row of the list: the button and five more
    constexpr uint32_t kRows = 14;         // the game's rows, and how many the list shows at once
    constexpr uint32_t kGoldRow = 11;
    constexpr float kRowHeight = 20.0f;
    constexpr char kRowName[] = "WEAPON_CAMO_LIST_";
    constexpr char kOffsetVar[] = "ui_buttonNavGroupOffset";
    // Create a Class's two. The private match's editor of the default classes
    // has twins of them (..._gamesettings) laid out otherwise, with classes
    // stored elsewhere; those keep the game's list.
    constexpr const char* kMenus[] = { "cac_weapon_camo_primary", "cac_weapon_camo_secondary" };

    // The pictures that are Gold's (docs/textures.md), by the group that switches them.
    enum : uint32_t { kWeapon = 1, kPreview = 2, kBanner = 3 };
    constexpr struct { uint32_t group; uint64_t key; } kGoldImages[] = {
        { kWeapon, 0xa8357229dfcf400cull }, { kWeapon, 0x2f7e28ad8644733cull },
        { kWeapon, 0xd6caf021a5eda72aull }, { kPreview, 0x73c9431503b443a9ull },
        { kBanner, 0x6fd4265d799d5a74ull },
    };

    uint32_t Load(uint8_t* base, uint32_t at) { return PPC_LOAD_U32(at); }
    void Store(uint8_t* base, uint32_t at, uint32_t value) { PPC_STORE_U32(at, value); }
    const char* Text(uint8_t* base, uint32_t at) { return at ? reinterpret_cast<const char*>(base + at) : ""; }

    // Guest memory for the copies. A menu's is reused when the menu is loaded
    // again: nothing of the old copies is named by the new menu.
    struct Arena
    {
        struct Block { uint32_t at, size; };
        std::vector<Block> blocks;
        size_t current = 0;
        uint32_t used = 0;
        bool failed = false;

        void Rewind() { current = 0; used = 0; failed = false; }
        bool Holds(uint32_t address) const
        {
            return std::any_of(blocks.begin(), blocks.end(), [address](const Block& block) {
                return address >= block.at && address < block.at + block.size;
            });
        }
        // Zeroed. On failure, an address in the first block (so that writes
        // stay harmless) and `failed`, which the caller checks once at the end.
        uint32_t Take(uint8_t* base, uint32_t size)
        {
            constexpr uint32_t kBlock = 256 << 10;
            size = (size + 3) & ~3u;
            while (current < blocks.size() && used + size > blocks[current].size) { current++; used = 0; }
            if (current == blocks.size())
            {
                const uint32_t bytes = std::max(size, kBlock);
                const uint32_t at = kernel::AllocateGuest(bytes);
                if (!at)
                {
                    failed = true;
                    current = 0;
                    used = 0;
                    if (blocks.empty() || blocks[0].size < size) return 0;
                }
                else
                {
                    blocks.push_back({ at, bytes });
                }
            }
            const uint32_t at = blocks[current].at + used;
            used += size;
            std::memset(base + at, 0, size);
            return at;
        }
        uint32_t Copy(uint8_t* base, uint32_t from, uint32_t size)
        {
            const uint32_t at = Take(base, size);
            if (at) std::memcpy(base + at, base + from, size);
            return at;
        }
        uint32_t String(uint8_t* base, const std::string& text)
        {
            const uint32_t at = Take(base, uint32_t(text.size() + 1));
            if (at) std::memcpy(base + at, text.c_str(), text.size());
            return at;
        }
    };

    // A camouflage as the menu knows it.
    struct Listed
    {
        camo::Camo camo;
        uint32_t ref = 0;            // "cc<nnn>", in guest memory, as the rest are
        uint32_t name = 0;           // in capitals, as the menu writes the game's
        uint32_t description = 0;
    };

    // The script of a copy's action, which chooses camouflage `index`, by
    // where its text is.
    struct Script { uint32_t at, size; size_t index; };

    struct State
    {
        bool started = false;
        std::vector<Listed> listed;
        Arena fixed;                  // what every menu's copies share
        Arena menus[std::size(kMenus)];
        std::vector<Script> scripts[std::size(kMenus)];
        bool refused[std::size(kMenus)] = {};   // said once, not at every menu list added
        uint32_t gold = 0, empty = 0, goldMaterial = 0, rowZero = 0, scratch = 0;
        // For StoredCamo: its operands and result, and two names of its path.
        uint32_t probe = 0, weaponSetups = 0, camoField = 0;
        // `<class>.weaponSetups.<n>` -> `<pack>/<camo>`, and `active` -> the
        // class chosen in a match; saves/custom_camos.txt.
        std::map<std::string, std::string> chosen;
        // What the banner last showed for a primary weapon, so that a
        // secondary's is shown only when the primary has none.
        bool primaryHasBanner = false;
        // The camouflage whose unlock (the table's column 6) the menu last
        // looked up: the preview pane's, when its material is asked for next.
        std::string lastUnlockOf;
    };
    State g;

    constexpr uint32_t kScratchSize = 128;
    // The reader's operands: ten of 8 bytes {type, value}, 2 a string and 0
    // an integer, their count at +0x50; its result, {type, value}, is put
    // after them.
    constexpr uint32_t kProbeCount = 0x50, kProbeResult = 0x58, kProbeSize = 0x60;
    constexpr char kActive[] = "active";

    fs::path SaveFile() { return kernel::SaveRoot() / "custom_camos.txt"; }

    void LoadChoices()
    {
        std::ifstream in(SaveFile());
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t equals = line.find('=');
            if (equals == std::string::npos || equals == 0) continue;
            g.chosen[line.substr(0, equals)] = line.substr(equals + 1);
        }
    }

    void SaveChoices()
    {
        std::error_code ec;
        fs::create_directories(kernel::SaveRoot(), ec);
        std::ofstream out(SaveFile(), std::ios::trunc);
        for (const auto& [slot, key] : g.chosen) out << slot << '=' << key << '\n';
        if (!out) LOGW("camos: cannot write %s; the choice lasts for this run only", SaveFile().string().c_str());
    }

    // The listed camouflage recorded for `slot`; null when there is none, or
    // its pack is gone.
    const Listed* ChosenFor(const std::string& slot)
    {
        const auto found = g.chosen.find(slot);
        if (found == g.chosen.end()) return nullptr;
        for (const Listed& one : g.listed)
            if (one.camo.key == found->second) return &one;
        return nullptr;
    }

    // "cc<nnn>" as a listed camouflage.
    const Listed* ByRef(const char* text)
    {
        if (text[0] != 'c' || text[1] != 'c') return nullptr;
        size_t index = 0;
        for (int i = 2; i < 5; i++)
        {
            if (text[i] < '0' || text[i] > '9') return nullptr;
            index = index * 10 + size_t(text[i] - '0');
        }
        return !text[5] && index < g.listed.size() ? &g.listed[index] : nullptr;
    }

    constexpr char kWeaponOf[] = "#weapon";   // `<slot>#weapon`: the weapon the class has there

    // What the weapons wear, said when it changes: a log's answer to "which
    // camouflage was that".
    constexpr char kOwnGold[] = "the game's Gold";
    void SayWeapon(std::string now)
    {
        static std::string said = kOwnGold;
        if (now == said) return;
        LOGI("camos: the weapon image shows %s", now.c_str());
        said = std::move(now);
    }

    // What the images of `group` show: the camouflage's picture for it, or
    // with none the game's own. `slot` is the class's weapon slot the
    // camouflage is on, for a camouflage with a picture a weapon.
    void Show(uint32_t group, const Listed* one, const std::string& slot = {})
    {
        static const std::vector<fs::path> none;
        if (!one)
        {
            gpu::replace::Select(group, none, 12.0);
            if (group == kWeapon) SayWeapon(kOwnGold);
            return;
        }
        const camo::Camo& camo = one->camo;
        const std::vector<fs::path>* pictures =
            group == kPreview ? &camo.swatch : group == kBanner ? &camo.banner : &camo.frames;
        std::vector<fs::path> own;
        const auto weapon = group == kWeapon && !camo.weapons.empty() ? g.chosen.find(slot + kWeaponOf) : g.chosen.end();
        if (weapon != g.chosen.end())
            for (const auto& [name, picture] : camo.weapons)
                if (!strcasecmp(name.c_str(), weapon->second.c_str()))
                {
                    own.assign(1, picture);
                    pictures = &own;
                }
        // A still picture that moves does so on the weapon and in the
        // preview; the banner, three times a weapon image's texels, stays still.
        using gpu::replace::Effect;
        const Effect effect = pictures->size() != 1 || group == kBanner ? Effect::None
                            : camo.animation == camo::Animation::Scroll ? Effect::Scroll
                            : camo.animation == camo::Animation::Shimmer ? Effect::Shimmer
                            : camo.animation == camo::Animation::Hue ? Effect::Hue
                            : Effect::None;
        // The weapon image's alpha is Gold's gloss (183 of 255 on every
        // texel): a mod's maps are opaque, which read as gloss is a mirror's
        // and leaves the weapon dark but for pinpoints of light, so the
        // picture is drawn with Gold's.
        using gpu::replace::Alpha;
        // An effect's loop of gpu::replace::kEffectFrames is three seconds.
        gpu::replace::Select(group, *pictures, effect == Effect::None ? camo.fps : 16.0, effect,
                             group == kWeapon ? Alpha::Title : Alpha::Picture);
        if (group != kWeapon) return;
        SayWeapon(camo.key + (own.empty() ? "" : ", its picture for " + weapon->second) +
                  (effect == Effect::None ? "" : ", animated"));
    }

    // The weapon's image: the camouflage of the class chosen in the match.
    void ShowWeapon()
    {
        const auto active = g.chosen.find(kActive);
        const Listed* one = nullptr;
        std::string slot;
        if (active != g.chosen.end())
            for (const char* weapon : { ".weaponSetups.0", ".weaponSetups.1" })
                if (!one)
                {
                    slot = active->second + weapon;
                    one = ChosenFor(slot);
                }
        Show(kWeapon, one, slot);
    }

    // Once, when the menus are first added: by then the game folder is known
    // and none of Gold's images has been read.
    bool Started(uint8_t* base)
    {
        if (g.started) return !g.listed.empty();
        g.started = true;
        // On unless switched off: a folder of packs is itself the request.
        if (const char* off = env::Text("MW2_CUSTOM_CAMOS"); off && !env::Flag("MW2_CUSTOM_CAMOS"))
        {
            LOGI("camos: MW2_CUSTOM_CAMOS=0: the camo packs are not listed");
            return false;
        }
        std::vector<camo::Camo> camos = camo::Read(kernel::GameRoot() / "camos");
        if (camos.empty()) return false;
        // A copy's name has three digits.
        constexpr size_t kMost = 1000;
        if (camos.size() > kMost)
        {
            LOGW("camos: only the first %zu of %zu camos are listed", kMost, camos.size());
            camos.resize(kMost);
        }

        g.gold = g.fixed.String(base, "gold");
        g.empty = g.fixed.String(base, "");
        g.goldMaterial = g.fixed.String(base, "ui_camoskin_gold");
        g.rowZero = g.fixed.String(base, "11");
        g.scratch = g.fixed.Take(base, kScratchSize);
        g.probe = g.fixed.Take(base, kProbeSize);
        g.weaponSetups = g.fixed.String(base, "weaponSetups");
        g.camoField = g.fixed.String(base, "camo");
        for (size_t i = 0; i < camos.size(); i++)
        {
            Listed one;
            one.camo = std::move(camos[i]);
            char ref[8];
            std::snprintf(ref, sizeof ref, "cc%03zu", i);
            one.ref = g.fixed.String(base, ref);
            std::string name = one.camo.label;
            for (char& c : name) c = char(std::toupper(uint8_t(c)));
            one.name = g.fixed.String(base, name);
            one.description = g.fixed.String(
                base, one.camo.name + ", from the pack \"" + one.camo.pack + "\"" +
                          (one.camo.author.empty() ? std::string() : " by " + one.camo.author) + ".");
            g.listed.push_back(std::move(one));
        }
        if (g.fixed.failed)
        {
            LOGW("camos: no guest memory for the custom camos; they are not listed");
            g.listed.clear();
            return false;
        }
        for (const auto& image : kGoldImages) gpu::replace::Switchable(image.group, image.key);
        LoadChoices();
        ShowWeapon();
        LOGI("camos: %zu custom camos are listed below Gold in Create a Class", g.listed.size());
        return true;
    }

    // ---- copying a row ----

    struct Entry { uint32_t kind, a, b; };

    Entry EntryAt(uint8_t* base, uint32_t entries, uint32_t index)
    {
        const uint32_t at = entries + index * kEntrySize;
        return { Load(base, at), Load(base, at + 4), Load(base, at + 8) };
    }
    bool IsOperator(const Entry& e, uint32_t op) { return e.kind == 0 && e.a == op; }
    bool IsInt(const Entry& e, uint32_t value) { return e.kind != 0 && e.a == 0 && e.b == value; }
    bool IsString(uint8_t* base, const Entry& e, const char* text)
    {
        return e.kind != 0 && e.a == 2 && e.b && !std::strcmp(Text(base, e.b), text);
    }

    // What changes in a copy of a row.
    struct Change
    {
        uint32_t fromRow, toRow;
        std::string fromName;        // the button's name, and its new one
        uint32_t toName;
        const Listed* custom;        // null for a row of the game's that only moves
        size_t index;                // of `custom` in the list
        size_t menu;
    };

    // A statement of `count` entries with nothing left of an evaluation.
    uint32_t NewStatement(uint8_t* base, Arena& arena, uint32_t count, uint32_t supporting)
    {
        const uint32_t at = arena.Take(base, kStatementSize + count * kEntrySize);
        if (!at) return 0;
        Store(base, at, count);
        Store(base, at + 4, at + kStatementSize);
        Store(base, at + 8, supporting);
        for (uint32_t word = 0x10; word < 0x30; word += 4) Store(base, at + word, 0xFFFFFFFFu);
        return at;
    }
    void SetEntry(uint8_t* base, uint32_t statement, uint32_t index, const Entry& e)
    {
        const uint32_t at = statement + kStatementSize + index * kEntrySize;
        Store(base, at, e.kind);
        Store(base, at + 4, e.a);
        Store(base, at + 8, e.b);
    }

    // A statement that is one string.
    uint32_t Literal(uint8_t* base, Arena& arena, uint32_t text)
    {
        const uint32_t at = NewStatement(base, arena, 1, 0);
        if (at) SetEntry(base, at, 0, { 1, 2, text });
        return at;
    }

    // What an entry of a row's statement becomes in the copy. The row's number
    // is a constant in four shapes, and the camouflage's place among the
    // game's (below 12: one every player has) in a fifth:
    //     20 * row            the row's height
    //     <statement> = row   what the button's onFocus sets the group's current row to
    //     ... != row )        the marker's test of the current row
    //     ( row )             the window test's two uses
    //     ( ( row < 12        a camouflage of the game's own
    // Other constants that happen to equal the row (a table's column) are not it.
    Entry Changed(uint8_t* base, const Change& change, uint32_t entries, uint32_t count, uint32_t i)
    {
        Entry e = EntryAt(base, entries, i);
        if (e.kind == 0) return e;
        if (e.a == 2)
        {
            if (change.custom && IsString(base, e, "gold")) e.b = change.custom->ref;
            else if (IsString(base, e, change.fromName.c_str())) e.b = change.toName;
            return e;
        }
        if (!IsInt(e, change.fromRow)) return e;
        const Entry before = i ? EntryAt(base, entries, i - 1) : Entry{ 0, 0, 0 };
        const Entry after = i + 1 < count ? EntryAt(base, entries, i + 1) : Entry{ 0, 0, 0 };
        const bool place = count == 1
            || (IsOperator(before, kTimes) && i >= 2 && IsInt(EntryAt(base, entries, i - 2), uint32_t(kRowHeight)))
            || (IsOperator(before, kNotEqual) && IsOperator(after, kClose))
            || (IsOperator(before, kOpen) && IsOperator(after, kClose));
        if (place) e.b = change.toRow;
        else if (change.custom && IsOperator(after, kLess) && i + 2 < count &&
                 IsInt(EntryAt(base, entries, i + 2), 12))
            e.b = 0;
        return e;
    }

    // The statement as the copy has it: the same one when nothing in it changes.
    uint32_t CopyStatement(uint8_t* base, Arena& arena, const Change& change, uint32_t statement)
    {
        if (!statement) return 0;
        const uint32_t count = Load(base, statement), entries = Load(base, statement + 4);
        bool differs = false;
        for (uint32_t i = 0; i < count && !differs; i++)
        {
            const Entry was = EntryAt(base, entries, i), now = Changed(base, change, entries, count, i);
            differs = was.b != now.b;
        }
        if (!differs) return statement;
        const uint32_t copy = NewStatement(base, arena, count, Load(base, statement + 8));
        if (!copy) return statement;
        for (uint32_t i = 0; i < count; i++) SetEntry(base, copy, i, Changed(base, change, entries, count, i));
        return copy;
    }

    // The window test, as the "new" marker of every row has it:
    //     && ( ( ( offset + ( 14 - 1 ) ) >= ( row ) ) && ( ( ( row ) - offset ) >= 0 ) )
    std::vector<Entry> WindowTest(uint32_t row, uint32_t offsetName)
    {
        const Entry open{ 0, kOpen, 0 }, close{ 0, kClose, 0 }, offset{ 0, kLocalVarInt, 0 },
            name{ 1, 2, offsetName }, rowIs{ 1, 0, row };
        return {
            { 0, kAnd, 0 }, open, open, open, offset, name, close, { 0, kPlus, 0 }, open,
            { 1, 0, kRows }, { 0, kMinus, 0 }, { 1, 0, 1 }, close, close, { 0, kAtLeast, 0 }, open, rowIs,
            close, { 0, kAnd, 0 }, open, open, open, rowIs, close, { 0, kMinus, 0 }, offset, name, close,
            close, { 0, kAtLeast, 0 }, { 1, 0, 0 }, close, close, close,
        };
    }

    // How many parentheses a statement leaves open at its end, which the
    // title's own do freely.
    int Unclosed(uint8_t* base, uint32_t statement)
    {
        const uint32_t count = Load(base, statement), entries = Load(base, statement + 4);
        int open = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            const Entry e = EntryAt(base, entries, i);
            if (e.kind != 0) continue;
            if (e.a == kOpen || e.a >= kFirstFunction) open++;
            else if (e.a == kClose) open--;
        }
        return open;
    }

    // `statement` and the window test of `row`: ( ( statement ) && test.
    // The parentheses it leaves open are closed here, so that the test
    // applies to all of it.
    uint32_t Windowed(uint8_t* base, Arena& arena, uint32_t statement, uint32_t row, uint32_t offsetName)
    {
        if (!statement) return 0;
        const uint32_t count = Load(base, statement), entries = Load(base, statement + 4);
        const int open = Unclosed(base, statement);
        if (open < 0) return statement;
        const std::vector<Entry> test = WindowTest(row, offsetName);
        const uint32_t total = 2 + count + uint32_t(open) + 1 + uint32_t(test.size());
        const uint32_t made = NewStatement(base, arena, total, Load(base, statement + 8));
        if (!made) return statement;
        uint32_t at = 0;
        SetEntry(base, made, at++, { 0, kOpen, 0 });
        SetEntry(base, made, at++, { 0, kOpen, 0 });
        for (uint32_t i = 0; i < count; i++) SetEntry(base, made, at++, EntryAt(base, entries, i));
        for (int i = 0; i <= open; i++) SetEntry(base, made, at++, { 0, kClose, 0 });
        for (const Entry& e : test) SetEntry(base, made, at++, e);
        return made;
    }

    // A button's height, with the row put far below the screen while it is
    // outside the window: ( ( height ) + ( ( ! ( test ) ) * 4000 ) ). A
    // button draws nothing of its own, but it must stay visible to take the
    // focus, which is what scrolls the list to it; out of reach of a pointer,
    // it takes it from the pad's up and down alone.
    uint32_t Displaced(uint8_t* base, Arena& arena, uint32_t statement, uint32_t row, uint32_t offsetName)
    {
        const uint32_t count = Load(base, statement), entries = Load(base, statement + 4);
        const int open = Unclosed(base, statement);
        if (open < 0) return statement;
        std::vector<Entry> test = WindowTest(row, offsetName);
        test.erase(test.begin());   // the && it is joined by elsewhere
        const Entry opens{ 0, kOpen, 0 }, closes{ 0, kClose, 0 };
        std::vector<Entry> made{ opens, opens };
        for (uint32_t i = 0; i < count; i++) made.push_back(EntryAt(base, entries, i));
        for (int i = 0; i <= open; i++) made.push_back(closes);
        made.insert(made.end(), { { 0, kPlus, 0 }, opens, opens, { 0, kNot, 0 }, opens });
        made.insert(made.end(), test.begin(), test.end());
        made.insert(made.end(), { closes, closes, { 0, kTimes, 0 }, { 1, 0, 4000 }, closes });
        const uint32_t at = NewStatement(base, arena, uint32_t(made.size()), Load(base, statement + 8));
        if (!at) return statement;
        for (size_t i = 0; i < made.size(); i++) SetEntry(base, at, uint32_t(i), made[i]);
        return at;
    }

    bool Mentions(uint8_t* base, uint32_t statement, const char* text)
    {
        const uint32_t count = Load(base, statement), entries = Load(base, statement + 4);
        for (uint32_t i = 0; i < count; i++)
            if (IsString(base, EntryAt(base, entries, i), text)) return true;
        return false;
    }

    // A handler set as the copy has it: the same one when nothing in it changes.
    uint32_t CopyHandlers(uint8_t* base, Arena& arena, const Change& change, uint32_t set, bool action)
    {
        if (!set) return 0;
        const uint32_t count = Load(base, set), list = Load(base, set + 4);
        std::vector<uint32_t> handlers(count);
        bool differs = false;
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t handler = Load(base, list + i * 4);
            const uint32_t data = Load(base, handler);
            const uint8_t type = base[handler + 4];
            uint32_t now = data;
            if (type == kScript)
            {
                // The script the hook on setPlayerData tells a copy by. Its
                // text stays as it is: "gold" is what the stats hold.
                const char* text = Text(base, data);
                if (change.custom && action && std::strstr(text, "\"camo\" , \"gold\""))
                {
                    const uint32_t size = uint32_t(std::strlen(text) + 1);
                    now = arena.Copy(base, data, size);
                    if (now) g.scripts[change.menu].push_back({ now, size, change.index });
                }
            }
            else if (type == kIf)
            {
                const uint32_t inner = CopyHandlers(base, arena, change, Load(base, data), action);
                const uint32_t condition = CopyStatement(base, arena, change, Load(base, data + 4));
                if (inner != Load(base, data) || condition != Load(base, data + 4))
                {
                    now = arena.Take(base, 8);
                    if (now) { Store(base, now, inner); Store(base, now + 4, condition); }
                }
            }
            else if (type == kElse)
            {
                now = CopyHandlers(base, arena, change, data, action);
            }
            else if (type >= kSetLocalFirst && type <= kSetLocalLast)
            {
                // The description is the pack's, whichever branch sets it.
                const bool describes = change.custom &&
                                       !std::strcmp(Text(base, Load(base, data)), "ui_selected_camo_desc");
                const uint32_t value = describes ? Literal(base, arena, change.custom->description)
                                                 : CopyStatement(base, arena, change, Load(base, data + 4));
                if (value != Load(base, data + 4))
                {
                    now = arena.Take(base, 8);
                    if (now) { Store(base, now, Load(base, data)); Store(base, now + 4, value); }
                }
            }
            handlers[i] = handler;
            if (now && now != data)
            {
                handlers[i] = arena.Copy(base, handler, 8);
                if (handlers[i]) Store(base, handlers[i], now);
                differs = true;
            }
        }
        if (!differs) return set;
        const uint32_t copy = arena.Take(base, 8), copies = arena.Take(base, count * 4);
        if (!copy || !copies) return set;
        Store(base, copy, count);
        Store(base, copy + 4, copies);
        for (uint32_t i = 0; i < count; i++) Store(base, copies + i * 4, handlers[i]);
        return copy;
    }

    uint32_t CopyItem(uint8_t* base, Arena& arena, const Change& change, uint32_t item)
    {
        const uint32_t copy = arena.Copy(base, item, kItemSize);
        if (!copy) return item;
        if (!std::strcmp(Text(base, Load(base, item + kItemName)), change.fromName.c_str()))
            Store(base, copy + kItemName, change.toName);
        // Where the row is before its expression has first run.
        const float down = kRowHeight * float(int32_t(change.toRow) - int32_t(change.fromRow));
        for (const uint32_t y : { kItemY, kItemGivenY })
        {
            uint32_t bits = Load(base, item + y);
            float value;
            std::memcpy(&value, &bits, sizeof value);
            value += down;
            std::memcpy(&bits, &value, sizeof bits);
            Store(base, copy + y, bits);
        }
        for (uint32_t client = 0; client < 4; client++)
            Store(base, copy + kItemFlags + client * 4, Load(base, item + kItemFlags + client * 4) & ~2u);

        constexpr uint32_t kAction = 4;   // of the nine: the one a press runs
        for (uint32_t i = 0; i < kItemHandlerCount; i++)
            Store(base, copy + kItemHandlers + i * 4,
                  CopyHandlers(base, arena, change, Load(base, item + kItemHandlers + i * 4), i == kAction));
        if (const uint32_t data = Load(base, item + kItemTypeData))
            Store(base, copy + kItemTypeData, arena.Copy(base, data, kItemTypeDataSize));
        if (const uint32_t count = Load(base, item + kItemFloatCount))
        {
            const uint32_t from = Load(base, item + kItemFloats);
            const uint32_t floats = arena.Copy(base, from, count * 8);
            if (floats)
            {
                for (uint32_t i = 0; i < count; i++)
                    Store(base, floats + i * 8 + 4, CopyStatement(base, arena, change, Load(base, from + i * 8 + 4)));
                Store(base, copy + kItemFloats, floats);
            }
        }
        for (const uint32_t statement : { kItemVisible, kItemDisabled, kItemMaterial })
            Store(base, copy + statement, CopyStatement(base, arena, change, Load(base, item + statement)));
        // The label looks its text up in the table by the camouflage's name;
        // a copy's is the pack's own.
        const uint32_t text = Load(base, item + kItemText);
        Store(base, copy + kItemText,
              change.custom && text && Mentions(base, text, "gold") ? Literal(base, arena, change.custom->name)
                                                                    : CopyStatement(base, arena, change, text));
        return copy;
    }

    std::string RowName(uint32_t row) { return kRowName + std::to_string(row); }

    // What keeps an item of `row` inside the list's window, as the words to
    // store in it: the button (the row's first item) by its height, the rest
    // by their visibility.
    using Stores = std::vector<std::pair<uint32_t, uint32_t>>;
    constexpr uint32_t kFloatY = 1;   // the rect's y, of what an item's float expressions drive
    void KeepInWindow(uint8_t* base, Arena& arena, uint32_t item, bool button, uint32_t row,
                      uint32_t offsetName, Stores& stores)
    {
        if (!button)
        {
            stores.emplace_back(item + kItemVisible,
                                Windowed(base, arena, Load(base, item + kItemVisible), row, offsetName));
            return;
        }
        const uint32_t count = Load(base, item + kItemFloatCount), floats = Load(base, item + kItemFloats);
        for (uint32_t i = 0; i < count; i++)
            if (Load(base, floats + i * 8) == kFloatY)
                stores.emplace_back(floats + i * 8 + 4,
                                    Displaced(base, arena, Load(base, floats + i * 8 + 4), row, offsetName));
    }

    // The name of the string the rows' expressions read the scroll offset by.
    uint32_t OffsetName(uint8_t* base, uint32_t item)
    {
        const uint32_t count = Load(base, item + kItemFloatCount), floats = Load(base, item + kItemFloats);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t statement = Load(base, floats + i * 8 + 4);
            const uint32_t entries = Load(base, statement + 4);
            for (uint32_t e = 0; e < Load(base, statement); e++)
                if (IsString(base, EntryAt(base, entries, e), kOffsetVar)) return EntryAt(base, entries, e).b;
        }
        return 0;
    }

    // Whether the menu is laid out as this file's head says: fourteen rows of
    // six items, in order, the last item of Gold's ending in the window test.
    // Null when it is, and what is not otherwise.
    const char* Unexpected(uint8_t* base, uint32_t list, uint32_t count, uint32_t& first, uint32_t& offsetName)
    {
        first = count;
        for (uint32_t i = 0; i < count && first == count; i++)
            if (!std::strcmp(Text(base, Load(base, Load(base, list + i * 4) + kItemName)), RowName(0).c_str()))
                first = i;
        if (first == count) return "it has no item named WEAPON_CAMO_LIST_0";
        if (first + kRows * kRowItems > count) return "it has fewer than fourteen rows of six items";
        for (uint32_t row = 0; row < kRows; row++)
            for (uint32_t i = 0; i < kRowItems; i++)
            {
                const uint32_t item = Load(base, list + (first + row * kRowItems + i) * 4);
                const char* name = Text(base, Load(base, item + kItemName));
                if (i == 0 ? std::strcmp(name, RowName(row).c_str()) != 0 : name[0] != 0)
                    return "its rows are not a named button and five unnamed items each";
                const uint32_t visible = Load(base, item + kItemVisible);
                if (!visible || Load(base, visible + 4) != visible + kStatementSize)
                    return "an item of a row has no visibility expression of the known form";
            }
        const uint32_t goldRow = first + kGoldRow * kRowItems;
        offsetName = OffsetName(base, Load(base, list + goldRow * 4));
        if (!offsetName) return "Gold's button is not placed by ui_buttonNavGroupOffset";
        const uint32_t marker = Load(base, Load(base, list + (goldRow + kRowItems - 1) * 4) + kItemVisible);
        const std::vector<Entry> test = WindowTest(kGoldRow, offsetName);
        const uint32_t entries = Load(base, marker + 4), length = Load(base, marker);
        if (length < test.size()) return "Gold's last item does not end in the window test";
        for (size_t i = 0; i < test.size(); i++)
        {
            const Entry has = EntryAt(base, entries, uint32_t(length - test.size() + i));
            const bool same = has.kind == test[i].kind && has.a == test[i].a &&
                              (has.kind == 0 || has.a == 2 ? true : has.b == test[i].b);
            if (!same) return "Gold's last item does not end in the window test";
        }
        return nullptr;
    }

    void Extend(uint8_t* base, uint32_t menu, size_t which)
    {
        const uint32_t count = Load(base, menu + kMenuItemCount), list = Load(base, menu + kMenuItems);
        Arena& arena = g.menus[which];
        if (arena.Holds(list)) return;   // extended already, and not loaded again since

        const uint32_t added = uint32_t(g.listed.size()) * kRowItems;
        uint32_t first = 0, offsetName = 0;
        const char* why = Unexpected(base, list, count, first, offsetName);
        if (why)
        {
            if (!g.refused[which])
                LOGW("camos: the menu %s is not laid out as expected (%s); its list is left as it is",
                     kMenus[which], why);
            g.refused[which] = true;
            return;
        }
        arena.Rewind();
        g.scripts[which].clear();
        const uint32_t made = arena.Take(base, (count + added) * 4);
        std::vector<uint32_t> items;
        items.reserve(count + added);
        Stores stores;
        const auto row = [&](uint32_t r) { return first + r * kRowItems; };
        const auto original = [&](uint32_t i) { return Load(base, list + i * 4); };

        for (uint32_t i = 0; i < row(kGoldRow + 1); i++) items.push_back(original(i));
        for (size_t k = 0; k < g.listed.size(); k++)
        {
            const uint32_t to = kGoldRow + 1 + uint32_t(k);
            const Change change{ kGoldRow, to, RowName(kGoldRow), arena.String(base, RowName(to)),
                                 &g.listed[k], k, which };
            for (uint32_t i = 0; i < kRowItems; i++)
            {
                items.push_back(CopyItem(base, arena, change, original(row(kGoldRow) + i)));
                KeepInWindow(base, arena, items.back(), i == 0, to, offsetName, stores);
            }
        }
        // The game's two rows after Gold, moved below the custom ones.
        for (uint32_t from = kGoldRow + 1; from < kRows; from++)
        {
            const uint32_t to = from + uint32_t(g.listed.size());
            const Change change{ from, to, RowName(from), arena.String(base, RowName(to)), nullptr, 0, which };
            for (uint32_t i = 0; i < kRowItems; i++)
            {
                items.push_back(CopyItem(base, arena, change, original(row(from) + i)));
                KeepInWindow(base, arena, items.back(), i == 0, to, offsetName, stores);
            }
        }
        for (uint32_t i = row(kRows); i < count; i++) items.push_back(original(i));

        // The rows that stay where they are.
        for (uint32_t r = 0; r <= kGoldRow; r++)
            for (uint32_t i = 0; i < kRowItems; i++)
                KeepInWindow(base, arena, original(row(r) + i), i == 0, r, offsetName, stores);

        if (arena.failed || !made)
        {
            LOGW("camos: no guest memory to extend the menu %s; its list is left as it is", kMenus[which]);
            g.scripts[which].clear();
            return;
        }
        // Nothing of the menu was changed until here.
        for (const auto& [at, value] : stores) Store(base, at, value);
        for (size_t i = 0; i < items.size(); i++) Store(base, made + uint32_t(i) * 4, items[i]);
        Store(base, menu + kMenuItems, made);
        Store(base, menu + kMenuItemCount, uint32_t(items.size()));
        LOGI("camos: %s lists %zu custom camos (%u items, were %u)", kMenus[which], g.listed.size(),
             uint32_t(items.size()), count);
    }

    // ---- the menu's commands ----

    // A local variable of the menus, by name; 0 when there is none.
    uint32_t LocalVar(PPCContext& ctx, uint8_t* base, uint32_t context, const std::string& name)
    {
        if (name.size() >= kScratchSize) return 0;
        std::memcpy(base + g.scratch, name.c_str(), name.size() + 1);
        // The caller's frame is in use above r1; the call gets one of its own below.
        const PPCContext saved = ctx;
        ctx.r1.u64 = saved.r1.u32 - 0x100;
        ctx.r3.u64 = context + kContextLocalVars;
        ctx.r4.u64 = g.scratch;
        GUEST_FUNC(T_UI_FindLocalVar)(ctx, base);
        const uint32_t variable = ctx.r3.u32;
        ctx = saved;
        return variable;
    }

    // The camouflage the stats hold for a class's weapon, by the menus' own
    // reader and for the local client the menu's command is for; empty when
    // it answers no string.
    std::string StoredCamo(PPCContext& ctx, uint8_t* base, const std::string& classes, const std::string& index,
                           const std::string& weapon)
    {
        if (classes.size() >= kScratchSize) return {};
        std::memcpy(base + g.scratch, classes.c_str(), classes.size() + 1);
        const auto operand = [&](uint32_t i, uint32_t type, uint32_t value) {
            Store(base, g.probe + i * 8, type);
            Store(base, g.probe + i * 8 + 4, value);
        };
        operand(0, 2, g.scratch);
        operand(1, 0, uint32_t(std::strtoul(index.c_str(), nullptr, 10)));
        operand(2, 2, g.weaponSetups);
        operand(3, 0, uint32_t(std::strtoul(weapon.c_str(), nullptr, 10)));
        operand(4, 2, g.camoField);
        Store(base, g.probe + kProbeCount, 5);
        const uint32_t result = g.probe + kProbeResult;
        Store(base, result, 0);
        Store(base, result + 4, 0);
        // The caller's frame is in use above r1; the call gets one of its own below.
        const PPCContext saved = ctx;
        ctx.r1.u64 = saved.r1.u32 - 0x100;
        ctx.r4.u64 = g.probe;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = result;
        GUEST_ORIG(T_UI_GetPlayerData)(ctx, base);
        ctx = saved;
        return Load(base, result) == 2 && Load(base, result + 4) ? Text(base, Load(base, result + 4)) : std::string();
    }

    // An argument of a menu command: a quoted string, a number, or
    // "localVarString" ( "name" ) / "localVarInt" ( "name" ), read as its value.
    struct Argument
    {
        std::string text;
        uint32_t variable = 0;   // the local variable it was read from, if one
    };

    // The arguments up to the closing parenthesis; false for anything else
    // than the shapes above, which a path to a class's camouflage never is.
    bool Arguments(PPCContext& ctx, uint8_t* base, uint32_t context, const char* at, std::vector<Argument>& out)
    {
        const auto space = [&] { while (*at == ' ' || *at == '\t') at++; };
        const auto quoted = [&](std::string& text) {
            if (*at != '"') return false;
            const char* end = std::strchr(at + 1, '"');
            if (!end) return false;
            text.assign(at + 1, end);
            at = end + 1;
            return true;
        };
        for (;;)
        {
            Argument one;
            space();
            if (*at == '"')
            {
                if (!quoted(one.text)) return false;
                space();
                if (*at == '(')
                {
                    const bool isInt = !strcasecmp(one.text.c_str(), "localVarInt");
                    if (!isInt && strcasecmp(one.text.c_str(), "localVarString")) return false;
                    std::string name;
                    at++;
                    space();
                    if (!quoted(name)) return false;
                    space();
                    if (*at++ != ')') return false;
                    one.variable = LocalVar(ctx, base, context, name);
                    if (!one.variable) return false;
                    const uint32_t type = Load(base, one.variable), value = Load(base, one.variable + 8);
                    if (type == 1) return false;
                    one.text = type == 0 ? std::to_string(int32_t(value)) : std::string(Text(base, value));
                    if (type == 0) one.variable = 0;
                }
            }
            else
            {
                const char* start = at;
                while (*at == '-' || (*at >= '0' && *at <= '9')) at++;
                if (at == start) return false;
                one.text.assign(start, at);
            }
            out.push_back(std::move(one));
            space();
            if (*at == ')') return true;
            if (*at++ != ',' || out.size() > 8) return false;
        }
    }

    bool IsNumber(const std::string& text)
    {
        return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    }

    // The copy whose script `text` is within; null for a script of the game's.
    const Script* CopyRunning(uint32_t text)
    {
        for (const auto& scripts : g.scripts)
            for (const Script& script : scripts)
                if (text >= script.at && text < script.at + script.size) return &script;
        return nullptr;
    }

    // `<class>.weaponSetups.<n>` when the path names `field` of a class's weapon.
    bool WeaponSlot(const std::string& classes, const std::string& index, const std::string& member,
                    const std::string& weapon, const std::string& field, const char* wanted, std::string& slot)
    {
        if (strcasecmp(member.c_str(), "weaponSetups") || strcasecmp(field.c_str(), wanted) ||
            !IsNumber(index) || !IsNumber(weapon) || classes.empty())
            return false;
        slot = classes + "." + index + ".weaponSetups." + weapon;
        return true;
    }
    bool CamoSlot(const std::string& classes, const std::string& index, const std::string& member,
                  const std::string& weapon, const std::string& field, std::string& slot)
    {
        return WeaponSlot(classes, index, member, weapon, field, "camo", slot);
    }
}

// (context = r3, the menu list = r4)
GUEST_HOOK(T_UI_AddMenuList)
{
    const uint32_t context = ctx.r3.u32;
    GUEST_ORIG(T_UI_AddMenuList)(ctx, base);
    const bool camos = Started(base);
    const uint32_t count = std::min(Load(base, context + kContextMenuCount), kMostMenus);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t menu = Load(base, context + kContextMenus + i * 4);
        if (!menu) continue;
        const char* name = Text(base, Load(base, menu + kMenuName));
        mw3maps::MenuAdded(base, menu, name);
        for (size_t which = 0; camos && which < std::size(kMenus); which++)
            if (!std::strcmp(name, kMenus[which])) Extend(base, menu, which);
    }
}

// (table = r3, the column searched = r4, the value = r5, the column returned = r6)
GUEST_HOOK(T_StringTable_Lookup)
{
    const uint32_t table = ctx.r3.u32, value = ctx.r5.u32;
    const bool camos = !g.listed.empty() && table && value && ctx.r4.u32 == 1 &&
                       !strcasecmp(Text(base, Load(base, table)), "mp/camoTable.csv");
    const Listed* one = camos ? ByRef(Text(base, value)) : nullptr;
    // The preview pane looks up, every frame, for the row that has focus:
    // what unlocks it (column 6), then its material (column 4). The second
    // straight after the first for one camouflage is so the preview: its
    // image shows that row's camouflage, or the game's own for a row of the
    // game's. A banner's material is looked up alone, and a locked row's
    // unlock alone.
    if (camos && ctx.r6.u32 == 6)
    {
        g.lastUnlockOf = Text(base, value);
    }
    else if (camos)
    {
        if (ctx.r6.u32 == 4 && g.lastUnlockOf == Text(base, value)) Show(kPreview, one);
        g.lastUnlockOf.clear();
    }
    if (!one)
    {
        GUEST_ORIG(T_StringTable_Lookup)(ctx, base);
        return;
    }
    // A row the table does not have: the number and the materials are Gold's
    // (0: number, 4 and 5: materials), the name the pack's (1: reference, 2:
    // name); nothing unlocks it (3: description, which the copies do not
    // read, 6 and 7: what a camouflage that is bought needs).
    switch (ctx.r6.u32)
    {
    case 0: ctx.r3.u64 = g.rowZero; break;
    case 1: ctx.r3.u64 = one->ref; break;
    case 2: ctx.r3.u64 = one->name; break;
    case 4: case 5: ctx.r3.u64 = g.goldMaterial; break;
    default: ctx.r3.u64 = g.empty; break;
    }
}

// (local client = r3, context = r4, the arguments' text through r5)
GUEST_HOOK(T_UI_SetPlayerData)
{
    const uint32_t context = ctx.r4.u32;
    const uint32_t text = g.listed.empty() || !ctx.r5.u32 ? 0 : Load(base, ctx.r5.u32);
    std::vector<Argument> path;
    if (!text || !Arguments(ctx, base, context, Text(base, text), path))
    {
        GUEST_ORIG(T_UI_SetPlayerData)(ctx, base);
        return;
    }
    const Script* copy = CopyRunning(text);

    std::string slot;
    if (path.size() != 6 || !CamoSlot(path[0].text, path[1].text, path[2].text, path[3].text, path[4].text, slot))
    {
        GUEST_ORIG(T_UI_SetPlayerData)(ctx, base);
        return;
    }
    // A copy's action, or a value read back from a class that has a custom
    // camouflage (the menu's getPlayerData answers cc<nnn> for it), which
    // the stats are given as the Gold it is stored as.
    const Listed* custom = copy ? &g.listed[copy->index] : ByRef(path[5].text.c_str());
    // One of the game's own written where a camouflage is recorded: the
    // player's choice if the slot holds Gold or is given it. Anything else is
    // the menus tidying a class that is not Gold, which is what every class
    // is before the stats arrive.
    const auto recorded = custom ? g.chosen.end() : g.chosen.find(slot);
    const bool chosen = recorded != g.chosen.end() &&
                        (!strcasecmp(path[5].text.c_str(), "gold") ||
                         !strcasecmp(StoredCamo(ctx, base, path[0].text, path[1].text, path[3].text).c_str(), "gold"));
    const uint32_t variable = custom && !copy ? path[5].variable : 0;
    const uint32_t held = variable ? Load(base, variable + 8) : 0;
    if (variable) Store(base, variable + 8, g.gold);
    GUEST_ORIG(T_UI_SetPlayerData)(ctx, base);
    if (variable) Store(base, variable + 8, held);

    if (custom)
    {
        g.chosen[slot] = custom->camo.key;
    }
    else
    {
        if (!chosen) return;
        LOGI("camos: %s was %s and is \"%s\" now, as the menu set it", slot.c_str(), recorded->second.c_str(),
             path[5].text.c_str());
        g.chosen.erase(recorded);
    }
    SaveChoices();
    ShowWeapon();
}

// (context = r3, the item = r4, the arguments' text through r5)
GUEST_HOOK(T_UI_ScriptMenuResponse)
{
    // "custom<n>" is the changeclass menu's answer for custom class n, from
    // 1; any other class has no custom camouflage.
    const char* text = g.listed.empty() || !ctx.r5.u32 ? "" : Text(base, Load(base, ctx.r5.u32));
    while (*text == ' ') text++;
    if (*text == '"' && (!strncasecmp(text + 1, "custom", 6) || !strncasecmp(text + 1, "class", 5)))
    {
        const char* digits = text + 1 + (!strncasecmp(text + 1, "custom", 6) ? 6 : 5);
        const long number = std::strtol(digits, nullptr, 10);
        const bool isCustom = !strncasecmp(text + 1, "custom", 6) && number > 0;
        std::string classes = "customClasses";
        if (const uint32_t variable = isCustom ? LocalVar(ctx, base, ctx.r3.u32, "ui_customClassLoc") : 0)
            if (Load(base, variable) > 1 && Load(base, variable + 8)) classes = Text(base, Load(base, variable + 8));
        const std::string active = isCustom ? classes + "." + std::to_string(number - 1) : std::string();
        if (g.chosen[kActive] != active)
        {
            g.chosen[kActive] = active;
            SaveChoices();
        }
        ShowWeapon();
    }
    GUEST_ORIG(T_UI_ScriptMenuResponse)(ctx, base);
}

void mw3camo::PlayerDataRead(uint8_t* base, uint32_t operands, uint32_t first, uint32_t result)
{
    // <classes>, <n>, "weaponSetups", <n>, "camo": strings are type 2 and
    // integers type 0, in the path as in the result.
    if (g.listed.empty() || Load(base, operands + 0x50) != first + 5 || first > 4 || Load(base, result) != 2) return;
    const auto text = [&](uint32_t i) -> std::string {
        const uint32_t type = Load(base, operands + (first + i) * 8), value = Load(base, operands + (first + i) * 8 + 4);
        return type == 2 ? std::string(Text(base, value)) : type == 0 ? std::to_string(int32_t(value)) : std::string();
    };
    std::string slot;
    // The weapon a class has in a slot, noted as the menus read it: a
    // camouflage with a picture a weapon is drawn by it. "none" is no
    // weapon to draw, and what every slot answers before the stats arrive.
    if (WeaponSlot(text(0), text(1), text(2), text(3), text(4), "weapon", slot))
    {
        const std::string weapon = Text(base, Load(base, result + 4));
        if (!strcasecmp(weapon.c_str(), "none")) return;
        std::string& noted = g.chosen[slot + kWeaponOf];
        if (noted != weapon)
        {
            noted = weapon;
            SaveChoices();
            ShowWeapon();
        }
        return;
    }
    if (!CamoSlot(text(0), text(1), text(2), text(3), text(4), slot)) return;
    const bool gold = !strcasecmp(Text(base, Load(base, result + 4)), "gold");
    const Listed* custom = gold ? ChosenFor(slot) : nullptr;
    if (custom) Store(base, result + 4, custom->ref);

    // The banner behind a class's weapon is Gold's whenever that weapon's
    // camouflage is: the primary's, or the secondary's when the primary has
    // none of its own.
    const bool primary = text(3) == "0";
    if (primary) g.primaryHasBanner = gold;
    if (gold && (primary || !g.primaryHasBanner)) Show(kBanner, custom);
}

#else

void mw3camo::PlayerDataRead(uint8_t*, uint32_t, uint32_t, uint32_t) {}

#endif
