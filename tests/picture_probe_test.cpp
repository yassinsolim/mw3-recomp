// What vk::probe::Analyse says of the frames a run's first seconds are judged
// by (runtime/gpu/vulkan/picture_probe.h, FrameWatch.kt): one colour is flat,
// noise is unlike its neighbours nearly everywhere, and a picture -- flat
// areas, gradients, hard edges, text -- is neither.
#include "picture_probe.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool ok, const std::string& what)
    {
        if (ok) return;
        g_failures++;
        std::printf("FAILED: %s\n", what.c_str());
    }

    using Frame = std::vector<uint8_t>;
    Frame Make() { return Frame(vk::probe::kWidth * vk::probe::kHeight * 4, 0); }
    void Set(Frame& frame, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b)
    {
        uint8_t* p = frame.data() + (y * vk::probe::kWidth + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    }
}

int main()
{
    using namespace vk::probe;

    Frame black = Make();
    Picture p = Analyse(black.data());
    Check(p.flat && p.isolated == 0 && p.luma == 0 && p.ground == 1000, "black is one colour, all of it ground");

    Frame grey = Make();
    for (uint32_t y = 0; y < kHeight; y++)
        for (uint32_t x = 0; x < kWidth; x++) Set(grey, x, y, 15 + (x & 1), 18, 26);
    p = Analyse(grey.data());
    Check(p.flat, "the window's own colour, a count apart here and there, is one colour");

    // Noise: every sample its own colour, and no ground under it.
    Frame noise = Make();
    uint32_t state = 7;
    for (uint8_t& byte : noise) { state = state * 1664525u + 1013904223u; byte = uint8_t(state >> 24); }
    p = Analyse(noise.data());
    Check(!p.flat && p.isolated > 600 && p.ground < 50,
          "noise is unlike its neighbours (" + std::to_string(p.isolated) + "), with no ground (" + std::to_string(p.ground) + ")");

    // The player's screenshot: mostly black with coloured blocks one sample across, one in three.
    auto speckle = [](uint32_t seed) {
        Frame blocks = Make();
        uint32_t state = seed;
        for (uint32_t y = 0; y < kHeight; y++)
            for (uint32_t x = 0; x < kWidth; x++)
            {
                state = state * 1664525u + 1013904223u;
                if ((state >> 24) % 3 == 0) Set(blocks, x, y, uint8_t(state >> 16), uint8_t(state >> 8), uint8_t(state));
            }
        return blocks;
    };
    const Frame first = speckle(99), second = speckle(100);
    p = Analyse(first.data());
    Check(!p.flat && p.isolated >= 120 && p.ground >= 600,
          "sparse coloured blocks on black are speckle on a ground (" + std::to_string(p.isolated) + ", " + std::to_string(p.ground) + ")");
    Check(p.stable == p.isolated, "with no look before it, speckle counts as having stayed: one look accuses nothing");
    // Half a second later it is elsewhere...
    p = Analyse(second.data(), first.data());
    Check(p.isolated >= 120 && p.stable * 4 <= p.isolated,
          "speckle that moved (" + std::to_string(p.stable) + " of " + std::to_string(p.isolated) + " stayed)");
    // ...and a picture of the same numbers that stands still -- stars, small print -- is where it was.
    p = Analyse(first.data(), first.data());
    Check(p.stable == p.isolated, "specks that stand still all stayed");
    // The same specks in other colours did not stay: a sample has to be what it was.
    Frame recoloured = first;
    for (size_t i = 0; i < recoloured.size(); i += 4)
        if (recoloured[i] | recoloured[i + 1] | recoloured[i + 2]) { recoloured[i] ^= 0x80; recoloured[i + 1] ^= 0x80; }
    p = Analyse(recoloured.data(), first.data());
    Check(p.stable * 4 <= p.isolated, "specks of another colour at the same places moved (" + std::to_string(p.stable) + ")");

    // A picture: a sky's gradient over ground, a hard horizon, a logo's rectangle, a line of "text".
    Frame scene = Make();
    for (uint32_t y = 0; y < kHeight; y++)
        for (uint32_t x = 0; x < kWidth; x++)
        {
            if (y < 18) Set(scene, x, y, uint8_t(40 + y * 4), uint8_t(80 + y * 4), uint8_t(160 + y * 3));
            else Set(scene, x, y, uint8_t(70 + (x / 8) * 6), 60, 40);
        }
    for (uint32_t y = 6; y < 12; y++)
        for (uint32_t x = 20; x < 44; x++) Set(scene, x, y, 240, 240, 240);
    for (uint32_t x = 4; x < 60; x += 2) Set(scene, x, 30, 255, 255, 255);
    p = Analyse(scene.data());
    Check(!p.flat && p.isolated < 60 && p.ground < 333,
          "a picture with edges and text is neither (" + std::to_string(p.isolated) + ", " + std::to_string(p.ground) + ")");

    // White text on black, a loading screen: not flat, its ground nearly everything, hardly a speck.
    Frame loading = Make();
    for (uint32_t x = 10; x < 50; x += 3) { Set(loading, x, 17, 255, 255, 255); Set(loading, x, 18, 255, 255, 255); }
    p = Analyse(loading.data());
    Check(!p.flat && p.isolated < 60 && p.ground > 900, "a loading screen's text is a picture (" + std::to_string(p.isolated) + ")");

    // The ground is the middle colour, whatever it is: speckle on blue.
    Frame blue = speckle(5);
    for (size_t i = 0; i < blue.size(); i += 4)
        if (!(blue[i] | blue[i + 1] | blue[i + 2])) blue[i + 2] = 200;
    p = Analyse(blue.data());
    Check(p.ground >= 600 && p.luma > 40, "a ground of any colour (" + std::to_string(p.ground) + ")");

    std::printf("%s\n", g_failures ? "picture probe: FAILED" : "picture probe: ok");
    return g_failures ? 1 : 0;
}
