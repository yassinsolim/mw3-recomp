// Which of Advanced Warfare's executables is given which of a player's
// packages (runtime/kernel/content_route.h). The packages are named here by
// the files at their roots; no game files are involved.
#include "runtime/kernel/content_route.h"

#include <cassert>
#include <cstdio>

using namespace kernel::content;

int main()
{
    // ---- which pack a package is, by its marker ------------------------------
    assert((PackOf({ "dlcinstall1.id", "common_mp.ff" }) == Pack{ Kind::InstallDisc, 1 }));
    assert((PackOf({ "mp_dam.ff", "DLC1.ID" }) == Pack{ Kind::MapPack, 1 }));
    assert((PackOf({ "dlccompat6.id" }) == Pack{ Kind::CompatibilityPack, 6 }));
    assert((PackOf({ "seasonpass.id" }) == Pack{ Kind::SeasonPass, 0 }));
    assert((PackOf({ "mdlc4.id" }) == Pack{ Kind::Bonus, 4 }));
    // Not markers: no number, another ending, a name that only begins like one.
    assert(PackOf({ "dlc.id", "dlc1.idx", "dlcx1.id", "mp_dam.ff", "dlc123.id" }).kind == Kind::Unknown);
    assert(PackOf({}).kind == Kind::Unknown);

    const Candidate install1{ { Kind::InstallDisc, 1 } }, install2{ { Kind::InstallDisc, 2 } };
    const Candidate havoc{ { Kind::MapPack, 2 } }, compat3{ { Kind::CompatibilityPack, 3 } };
    const Candidate pass{ { Kind::SeasonPass, 0 } }, bonus{ { Kind::Bonus, 4 } }, unknown{};

    // ---- the disc's executables: the install disc's and the bonus items ------
    // A Compatibility Pack, which the campaign finds no marker it knows in
    // and notes as a corrupt one, is not listed to it.
    {
        const auto verdicts = Route({ install1, compat3, havoc, install2, pass, bonus, unknown }, false);
        assert(verdicts[0] == Verdict::Given && verdicts[3] == Verdict::Given && verdicts[5] == Verdict::Given);
        assert(verdicts[1] == Verdict::OtherExecutable && verdicts[2] == Verdict::OtherExecutable);
        assert(verdicts[4] == Verdict::OtherExecutable);
        assert(verdicts[6] == Verdict::NoMarker);
    }
    // Whatever a player has of the packs the disc's executables are given,
    // their list of twelve keeps its empty entry.
    {
        std::vector<Candidate> every{ install1, install2 };
        for (uint32_t n = 0; n <= 5; n++) every.push_back({ { Kind::Bonus, n } });
        size_t given = 0;
        for (const Verdict verdict : Route(every, false)) given += verdict == Verdict::Given;
        assert(given == every.size() && given <= kDiscListHolds);
    }
    // The tester's case: a player's packs found ahead of the install disc's
    // two, as the app finds them. The install disc's are listed first, for
    // either executable, and the others keep the order they were found in.
    {
        const std::vector<Candidate> found{ compat3, bonus, havoc, install1, unknown, pass, install2 };
        assert((ListOrder(found, Route(found, false)) == std::vector<size_t>{ 3, 6, 1 }));
        assert((ListOrder(found, Route(found, true)) == std::vector<size_t>{ 3, 6, 0, 1, 2, 5 }));
    }

    // ---- the updated multiplayer: every pack it can name ---------------------
    {
        const auto verdicts = Route({ install1, compat3, havoc, install2, pass, bonus, unknown }, true);
        for (size_t i = 0; i < 6; i++) assert(verdicts[i] == Verdict::Given);
        assert(verdicts[6] == Verdict::NoMarker);
    }

    // ---- the same pack twice -------------------------------------------------
    // Two versions: the higher is given, whichever was found first.
    {
        Candidate older = havoc, newer = havoc;
        older.version = 1; newer.version = 2;
        auto verdicts = Route({ older, newer }, true);
        assert(verdicts[0] == Verdict::OlderCopy && verdicts[1] == Verdict::Given);
        verdicts = Route({ newer, compat3, older }, true);
        assert(verdicts[0] == Verdict::Given && verdicts[1] == Verdict::Given && verdicts[2] == Verdict::OlderCopy);
    }
    // The same version under two names (a pack copied from two consoles): one of them.
    {
        const auto verdicts = Route({ compat3, compat3, compat3 }, true);
        assert(verdicts[0] == Verdict::Given && verdicts[1] == Verdict::OlderCopy && verdicts[2] == Verdict::OlderCopy);
    }
    // A copy the executable is not given anyway is not a duplicate to it.
    {
        const auto verdicts = Route({ havoc, havoc }, false);
        assert(verdicts[0] == Verdict::OtherExecutable && verdicts[1] == Verdict::OtherExecutable);
    }
    // Different packs of one kind are not copies of each other.
    {
        const auto verdicts = Route({ install1, install2, { { Kind::CompatibilityPack, 1 } }, compat3 }, true);
        for (const Verdict verdict : verdicts) assert(verdict == Verdict::Given);
    }

    assert(NameOf(compat3.pack) == "Compatibility Pack 3");
    std::printf("content_route_test: ok\n");
    return 0;
}
