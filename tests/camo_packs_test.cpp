// The camouflage pack reader (runtime/camo_packs.h): that a pack.json's
// camouflages come out in order with their pictures, name and credit; that a
// still and an animation are told apart and a preview defaults to the
// camouflage; and that what is not a pack -- a file that does not parse, a
// picture that is not there, a path out of the pack -- is left out rather
// than trusted.
//
// It asserts rather than reporting, so ctest passes or fails. The packs are
// written here, into a temporary folder: no game file is involved.
#include "runtime/camo_packs.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;
    int g_checks = 0;
    int g_failures = 0;

    void Check(bool ok, const std::string& what)
    {
        g_checks++;
        if (ok) return;
        g_failures++;
        std::fprintf(stdout, "FAIL: %s\n", what.c_str());
    }

    void Write(const fs::path& file, const std::string& text)
    {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
}

int main()
{
    const fs::path root = fs::temp_directory_path() / "mw3_camo_packs_test";
    fs::remove_all(root);

    // Two packs, read in the order of their folders' names.
    Write(root / "b-pack" / "pack.json", R"({
        "name": "Second \"quoted\" \u00e9",
        "author": "Somebody",
        "url": "https://example.org/pack",
        "permission": "with permission",
        "ignored": [1, 2.5, true, null, {"deep": []}],
        "camos": [
            { "id": "still", "name": "Still", "image": "still.png", "swatch": "still_swatch.png" },
            { "id": "moving", "name": "Moving", "frames": ["moving/0.png", "missing.png", "moving/1.png"], "fps": 500 },
            { "id": "gone", "name": "Gone", "image": "not-there.png" },
            { "id": "escapes", "name": "Escapes", "image": "../outside.png" },
            { "name": "No id", "image": "still.png" }
        ]
    })");
    for (const char* picture : { "still.png", "still_swatch.png", "moving/0.png", "moving/1.png" })
        Write(root / "b-pack" / picture, "x");
    Write(root / "outside.png", "x");
    Write(root / "a-pack" / "pack.json", R"({ "camos": [ { "id": "one", "image": "one.pnm" } ] })");
    Write(root / "a-pack" / "one.pnm", "x");
    Write(root / "c-broken" / "pack.json", R"({ "camos": [ { "id": "one", )");
    Write(root / "d-empty" / "pack.json", R"({ "name": "No list" })");
    Write(root / "not-a-folder.json", "{}");

    const std::vector<camo::Camo> camos = camo::Read(root);
    Check(camos.size() == 3, "three camos are read, got " + std::to_string(camos.size()));
    if (camos.size() == 3)
    {
        Check(camos[0].key == "a-pack/one" && camos[0].name == "one" && camos[0].pack == "a-pack",
              "a pack with no name is named by its folder, a camo with none by its id");
        Check(camos[0].frames.size() == 1 && camos[0].swatch == camos[0].frames && camos[0].fps == 12.0,
              "a still: one picture, which is its own preview");
        Check(camos[1].key == "b-pack/still" && camos[1].name == "Still" && camos[1].author == "Somebody",
              "the pack's credit is on its camos");
        Check(camos[1].pack == "Second \"quoted\" \xC3\xA9", "escapes in a string are read");
        Check(camos[1].swatch.size() == 1 && camos[1].swatch[0].filename() == "still_swatch.png",
              "a preview that is named is used");
        Check(camos[2].key == "b-pack/moving" && camos[2].frames.size() == 2,
              "an animation keeps the frames that are files, in order");
        Check(camos[2].frames.size() == 2 && camos[2].frames[1].filename() == "1.png", "the second frame is 1.png");
        Check(camos[2].fps == 120.0, "the rate is kept within bounds");
    }
    Check(camo::Read(root / "nowhere").empty(), "no folder, no camos");

    fs::remove_all(root);
    std::printf("camo_packs_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
