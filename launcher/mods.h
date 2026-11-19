#pragma once
// Mods, of two kinds.
//
// Extra game files (fastfiles, image archives, movies) are copied into the
// game folder. The engine reads that one flat directory, so a same-version
// .ff just loads -- maps, weapons and modes from packs like the CoD Xe
// fastfiles work this way.
//
// A script mod (Modern Warfare 3; docs/mods.md) is a folder with .gsc scripts
// in it. It is copied to <game>/mods/<its name>/ and named in mods.txt there,
// which is what the game reads (runtime/mods.cpp). Choosing a folder that is
// already inside <game>/mods turns that mod off, or on again.
//
// Executables are never taken from a mod, and the CoD Xe plugin's own files
// are reported as skipped: the plugin is a program for the console. The
// install behind this screen is untouched otherwise.
#include <filesystem>
#include <string>
#include <vector>

namespace mods
{
    struct Skipped
    {
        std::string name, reason;
    };
    // What happened to a script mod.
    enum class Change { None, Installed, TurnedOn, TurnedOff };
    struct Result
    {
        std::vector<std::string> copied;
        std::vector<Skipped> skipped;
        std::string mod;            // the script mod's name, when the folder was one
        Change change = Change::None;
        size_t modFiles = 0;        // how many of its files were copied
    };

    // Installs what `folder` holds: a script mod as a whole into the game's
    // mods folder, and the game's own kinds of file at its top level into the
    // game folder, leaving files already there, whole, alone.
    Result Install(const std::filesystem::path& folder, const std::filesystem::path& game);

    // The outcome as sentences for the menu.
    std::string Report(const Result& result);
}
