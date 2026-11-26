#pragma once
// Which of Advanced Warfare's executables is given which of a player's
// packages (content.cpp, docs/dlc.md).
//
// The game is three executables here: the disc's campaign, the disc's
// multiplayer, and the multiplayer as title update 17 makes it. Each lists
// the console's downloaded content, mounts every package and looks for the
// marker that says which pack it is. The disc's two know the markers of their
// own table (dlcinstall1 and 2, dlc1 to 5, seasonpass, mdlc0 to 5) and no
// other: a package with none of them (a Compatibility Pack, which came with
// the updates) is closed again and remembered as a corrupt one, and the menu
// then says "An installed Content Pack is corrupt. It must be deleted to
// continue." and offers only Exit. Their list also holds twelve packages and
// is read to the first empty entry, so a thirteenth is read out of whatever
// lies behind it. A console never shows either: it starts the updated
// executables once the update is there.
//
// So a package goes only to an executable that can use it. The install
// disc's two and the bonus items (mdlc) are files and entitlements of the
// game as the disc has it, and go to all three. The map packs, the season
// pass that entitles to them and the Compatibility Packs are built for the
// updated game, and go to it alone: the disc's multiplayer would list a
// pack's maps and fail to load them. A package with no marker at all is
// nobody's, and of two packages that are the same pack the newer is taken.
//
// No guest and no file is touched here, so the rules are tested as they are
// (tests/content_route_test.cpp).
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace kernel::content
{
    enum class Kind { Unknown, InstallDisc, MapPack, CompatibilityPack, SeasonPass, Bonus };

    struct Pack
    {
        Kind kind = Kind::Unknown;
        uint32_t number = 0;
        bool operator==(const Pack& other) const { return kind == other.kind && number == other.number; }
    };

    // The pack a package is, by the marker at its root: "<stem><n>.id".
    inline Pack PackOf(const std::vector<std::string>& rootFiles)
    {
        // The longer stem first where one begins another ("dlc", "dlcinstall").
        static constexpr struct { const char* stem; Kind kind; } kMarkers[] = {
            { "dlcinstall", Kind::InstallDisc }, { "dlccompat", Kind::CompatibilityPack },
            { "mdlc", Kind::Bonus }, { "dlc", Kind::MapPack },
        };
        for (const std::string& file : rootFiles)
        {
            std::string name;
            for (const char c : file) name += char(std::tolower(uint8_t(c)));
            if (name == "seasonpass.id") return { Kind::SeasonPass, 0 };
            if (name.size() < 4 || name.compare(name.size() - 3, 3, ".id") != 0) continue;
            name.resize(name.size() - 3);
            for (const auto& marker : kMarkers)
            {
                const std::string stem = marker.stem;
                if (name.compare(0, stem.size(), stem) != 0) continue;
                const std::string digits = name.substr(stem.size());
                bool number = !digits.empty() && digits.size() <= 2;
                for (const char c : digits) number = number && std::isdigit(uint8_t(c));
                if (number) return { marker.kind, uint32_t(std::atoi(digits.c_str())) };
            }
        }
        return {};
    }

    // What it is, for the log's sentence about it. A map pack's number is its
    // marker's and not the one in its name (the pack sold as Map Pack 4 has
    // dlc5.id), so the marker is what is said.
    inline std::string NameOf(const Pack& pack)
    {
        const std::string number = std::to_string(pack.number);
        switch (pack.kind)
        {
        case Kind::InstallDisc:       return "the install disc's package " + number;
        case Kind::MapPack:           return "a map pack (dlc" + number + ".id)";
        case Kind::CompatibilityPack: return "Compatibility Pack " + number;
        case Kind::SeasonPass:        return "the Season Pass";
        case Kind::Bonus:             return "a bonus item (mdlc" + number + ".id)";
        case Kind::Unknown:           break;
        }
        return "no pack this game knows";
    }

    // Whether the disc's executables are given it; the updated multiplayer is
    // given every pack it can name.
    inline bool DiscTakes(Kind kind) { return kind == Kind::InstallDisc || kind == Kind::Bonus; }
    inline bool UpdateTakes(Kind kind) { return kind != Kind::Unknown; }

    enum class Verdict { Given, OtherExecutable, NoMarker, OlderCopy };

    struct Candidate
    {
        Pack pack;
        uint32_t version = 0;
    };

    // What becomes of each package, in the order they were found, for the
    // updated multiplayer (`update`) or the disc's executables. Of several
    // that are one pack the highest version is given, the first found of
    // equals.
    inline std::vector<Verdict> Route(const std::vector<Candidate>& found, bool update)
    {
        std::vector<Verdict> verdicts(found.size(), Verdict::Given);
        for (size_t i = 0; i < found.size(); i++)
        {
            const Kind kind = found[i].pack.kind;
            if (kind == Kind::Unknown) { verdicts[i] = Verdict::NoMarker; continue; }
            if (!(update ? UpdateTakes(kind) : DiscTakes(kind))) { verdicts[i] = Verdict::OtherExecutable; continue; }
            for (size_t earlier = 0; earlier < i; earlier++)
            {
                if (verdicts[earlier] != Verdict::Given || !(found[earlier].pack == found[i].pack)) continue;
                if (found[i].version > found[earlier].version) verdicts[earlier] = Verdict::OlderCopy;
                else verdicts[i] = Verdict::OlderCopy;
                break;
            }
        }
        return verdicts;
    }

    // The most packages the disc's executables are listed: their list holds
    // twelve and is read to the first empty entry, which has to be in it.
    constexpr size_t kDiscListHolds = 11;
}
