#pragma once
// Camouflage packs: the pictures Modern Warfare 3's multiplayer lists below
// Gold in Create a Class (mw3_custom_camos.cpp, docs/textures.md).
//
// A pack is a folder `camos/<pack-id>/` in the game folder holding `pack.json`
// and the pictures it names:
//
//     {
//       "name": "Solid Colours",
//       "label": "Solid",
//       "author": "Sloth",
//       "url": "https://...",
//       "camos": [
//         { "id": "nebula", "name": "Nebula", "frames": ["nebula/0.png", "nebula/1.png"], "fps": 8 },
//         { "id": "gold", "name": "Rainbow", "target": "gold", "specular": "gold_spc.png",
//           "colour": "gold_col.png", "swatch": "gold_swatch.png", "banner": "gold_banner.png",
//           "animation": "scroll" },
//         { "id": "skin", "name": "Jewel", "image": "skin.png", "weapons": { "iw5_ak47": "skin/iw5_ak47.png" } }
//       ]
//     }
//
// What is drawn on the weapon is `frames` (an animation shown at `fps`, 12
// when absent), else `image`, else `specular`, else `colour`: Gold's image on
// the console, which every pack camouflage is drawn through, is a specular
// map, so a PC mod's specular map is the one that looks as it did there.
// `weapons` gives a weapon (as a class names it) a picture of its own.
// `swatch` (or `swatch_frames`) is the menu's preview and `banner` the strip
// behind the weapon; both default to the camouflage itself. `target` says
// which of the game's images a PC mod made the camouflage for ("gold",
// "weapon_camo_red", ...): it is only a note of where it came from, since no
// pack replaces anything. `animation` ("scroll", "shimmer" or "hue") moves a
// still picture. `label` is the pack's short name for the menu's list.
//
// Paths are relative to the pack's folder and may not leave it. Pictures are
// PNG or binary PNM, of any size: they are made the size of the texture they
// stand in for. A pack that does not parse, and a camouflage with no picture
// that exists, is logged and left out.
//
// `camos/options.txt` holds the player's choices about the packs, one a line:
//
//     off=<pack-id>                  the pack is not listed
//     animate=<pack-id>              its still camouflages scroll
//     animate=<pack-id>/<camo-id>    that one does
//     animate-gold=1                 every still camouflage made for Gold scrolls
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace camo
{
    enum class Animation { None, Scroll, Shimmer, Hue };

    struct Camo
    {
        // `<pack-id>/<camo-id>`: what a choice is saved as, so it survives
        // packs coming and going.
        std::string key;
        std::string name;
        std::string pack;      // the pack's name
        std::string label;     // as the menu's list shows it, the pack with it
        std::string author;
        std::vector<std::filesystem::path> frames;   // one for a still picture
        std::vector<std::filesystem::path> swatch;
        std::vector<std::filesystem::path> banner;
        // A weapon's own picture, by the weapon as a class names it.
        std::vector<std::pair<std::string, std::filesystem::path>> weapons;
        double fps = 12.0;
        // How a still picture moves: the pack's word, or the player's
        // (options.txt), which is always Scroll.
        Animation animation = Animation::None;
    };

    // Every camouflage of every pack in `folder` that options.txt does not
    // switch off, packs in the order of their folders' names and camouflages
    // in their pack's order.
    std::vector<Camo> Read(const std::filesystem::path& folder);
}
