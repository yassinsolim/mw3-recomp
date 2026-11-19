#pragma once
// Camouflage packs: the pictures Modern Warfare 3's multiplayer lists below
// Gold in Create a Class (mw3_custom_camos.cpp, docs/textures.md).
//
// A pack is a folder `camos/<pack-id>/` in the game folder holding `pack.json`
// and the pictures it names:
//
//     {
//       "name": "MW3 Recomp Originals",
//       "author": "the MW3 Recomp contributors",
//       "url": "https://...",
//       "permission": "Original procedural art; ...",
//       "camos": [
//         { "id": "nebula", "name": "Nebula", "frames": ["nebula/0.png", "nebula/1.png"], "fps": 8 },
//         { "id": "carbon", "name": "Carbon", "image": "carbon.png", "swatch": "carbon_swatch.png" }
//       ]
//     }
//
// `image` is one picture and `frames` an animation shown at `fps` (12 when
// absent); `swatch` is the menu's preview and defaults to the camouflage
// itself. Paths are relative to the pack's folder and may not leave it.
// Pictures are PNG or binary PNM, of any size: they are made the size of the
// texture they stand in for. A pack that does not parse, and a camouflage
// with no picture that exists, is logged and left out.
#include <filesystem>
#include <string>
#include <vector>

namespace camo
{
    struct Camo
    {
        // `<pack-id>/<camo-id>`: what a choice is saved as, so it survives
        // packs coming and going.
        std::string key;
        std::string name;      // as the menu shows it
        std::string pack;      // the pack's name
        std::string author;
        std::vector<std::filesystem::path> frames;   // one for a still picture
        std::vector<std::filesystem::path> swatch;
        double fps = 12.0;
    };

    // The most that are listed: a camouflage menu holds 512 items, 271 of
    // them the game's, and a camouflage is a row of six.
    constexpr size_t kMost = 40;

    // Every camouflage of every pack in `folder`, packs in the order of their
    // folders' names and camouflages in their pack's order.
    std::vector<Camo> Read(const std::filesystem::path& folder);
}
