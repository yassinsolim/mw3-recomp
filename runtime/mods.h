#pragma once
// Mods: folders of game scripts and raw files laid over the ones in the
// fastfiles (mods.cpp, docs/mods.md).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct PPCContext;

namespace mods
{
    // The mods' script or raw file for what the title asks its asset database
    // for (type, name in guest memory), 0 for the database's own: for a title
    // whose lookup another file hooks (Advanced Warfare's, aw_update_language.cpp).
    uint32_t FindAsset(PPCContext& ctx, uint8_t* base, uint32_t type, uint32_t name);

    // The script compiler (mods_gsc.cpp): gsc-tool's, for this console's
    // build of the game. The executable has no compiler of its own: its
    // scripts come compiled, as `scriptfile` assets.
    namespace script
    {
        // What a script the compiler asks for is: its source, or the two
        // halves of a compiled one (the code, and the names beside it).
        struct File
        {
            std::vector<uint8_t> text, bytecode, stack;
        };
        // (name without its extension, '/' between folders) -> whether there is one
        using Finder = std::function<bool(const std::string& name, File& file)>;
        // Forgets what earlier compiles read, so a file edited since is read again.
        void Begin();
        // Source to the two halves of a script file; false with `error` set.
        // `includes` answers for the scripts the source includes.
        bool Compile(const std::string& name, std::vector<uint8_t> text, const Finder& includes,
                     std::vector<uint8_t>& bytecode, std::vector<uint8_t>& stack, std::string& error);
        // A compiled script back to source, for MW2_SCRIPT_DUMP.
        bool Decompile(const std::vector<uint8_t>& bytecode, const std::vector<uint8_t>& stack,
                       std::vector<uint8_t>& text, std::string& error);
        // A stock script is named by a number in the fastfiles: its path for
        // the number ("maps/mp/_utility" for 82), empty when it has none, and
        // the number for a path, 0 when it has none.
        std::string NameOf(uint32_t id);
        uint32_t IdOf(const std::string& name);
    }
}
