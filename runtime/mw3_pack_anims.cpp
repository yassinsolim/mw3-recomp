// The animated models of a player's map packs, in Modern Warfare 3 (the
// multiplayer, and the campaign's executable for Special Ops).
//
// A map's swaying trees, cranes and lanterns are script models whose
// animations the map's scripts name in the animation tree "animated_props".
// The tree is a raw file of the game's common fastfile,
// animtrees/animated_props.atr: a list of animation names. The disc's has
// the animations of the disc's maps; those of a pack's map were added to it
// by the title update that came with the pack, which a player's packages do
// not hold. With the disc's list the level's load ends in "animation
// 'foliage_prk_tree_birch_sway' not defined in anim tree 'animated_props'"
// (Liberation; Overwatch and Sanctuary alike), and the title starts again
// at its menu.
//
// The animations themselves are in the map's own fastfile. So when the
// player has packs, the animations a level's scripts use of a tree and the
// tree's text does not name are added to the text, at the tree's root, as
// the update's list has them: a name on a line of its own. The root itself
// ("root", which every tree has and no text names) is not one of them.
// Nothing is added without packs, where a name the tree lacks is a script's
// mistake and the title's error says so.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "log.h"
#include "kernel/kernel.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#if defined(MW3_TITLE) && defined(T_Scr_AnimTreeParse)

namespace
{
    std::string Lower(std::string text)
    {
        for (char& c : text) c = char(std::tolower(uint8_t(c)));
        return text;
    }

    // The names a tree's text has: every word of it, braces and comments apart.
    std::set<std::string> NamesIn(const std::string& text)
    {
        std::set<std::string> names;
        std::string word;
        for (size_t i = 0; i <= text.size(); i++)
        {
            const char c = i < text.size() ? text[i] : ' ';
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
            {
                while (i < text.size() && text[i] != '\n') i++;
            }
            else if (std::isalnum(uint8_t(c)) || c == '_')
            {
                word += char(std::tolower(uint8_t(c)));
                continue;
            }
            if (!word.empty()) names.insert(std::move(word));
            word.clear();
        }
        return names;
    }

    // The animations the scripts use of the tree: the names of `names`' children.
    std::vector<std::string> Used(uint8_t* base, uint32_t names)
    {
        std::vector<std::string> used;
        const uint32_t instance = (names & 1) * T_SCR_INSTANCE;
        const uint32_t strings = PPC_LOAD_U32(T_DATA_ScrStrings);
        for (uint32_t child = PPC_LOAD_U16(T_DATA_ScrFirstChild + names * 4); child;)
        {
            const uint32_t entry = T_DATA_ScrChildren + (instance + child) * 16;
            const uint32_t string = (uint32_t(PPC_LOAD_U16(entry + 0xA)) << 8) + PPC_LOAD_U8(entry + 7);
            if (string) used.emplace_back(reinterpret_cast<const char*>(base + strings + string * T_SCR_STRING_SIZE + 4));
            child = PPC_LOAD_U16(entry + 0xC);
        }
        return used;
    }
}

GUEST_HOOK(T_Scr_AnimTreeParse)
{
    const uint32_t text = ctx.r3.u32, names = ctx.r5.u32;
    std::string added, first;
    if (text && names && !kernel::DownloadedContentFiles().empty())
    {
        const std::string tree = reinterpret_cast<const char*>(base + text);
        const std::set<std::string> has = NamesIn(tree);
        size_t count = 0;
        for (const std::string& name : Used(base, names))
            if (!has.count(Lower(name)) && Lower(name) != "root")
            {
                if (!count) first = name;
                added += "\n" + name;
                count++;
            }
        if (count)
        {
            // Kept, and used again by the next tree it is large enough for:
            // the title remembers where the text began, to say where a bad
            // token is.
            static uint32_t buffer = 0, room = 0;
            const uint32_t size = uint32_t(tree.size() + added.size() + 2);
            if (size > room)
            {
                room = size + 4096;
                buffer = kernel::AllocateGuest(room, 16);
            }
            if (buffer)
            {
                const std::string whole = tree + added + "\n";
                std::memcpy(base + buffer, whole.c_str(), whole.size() + 1);
                ctx.r3.u64 = buffer;
                LOGI("packs: %zu animations the level uses are added to its animation tree, which the disc's list lacks (the first: %s)",
                     count, first.c_str());
            }
            else
            {
                room = 0;
                LOGW("packs: no guest memory for the animation tree's %zu more animations; the level will not load", count);
            }
        }
    }
    GUEST_ORIG(T_Scr_AnimTreeParse)(ctx, base);
}

#endif
