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
    Check(p.flat && p.isolated == 0 && p.luma == 0, "black is one colour");

    Frame grey = Make();
    for (uint32_t y = 0; y < kHeight; y++)
        for (uint32_t x = 0; x < kWidth; x++) Set(grey, x, y, 15 + (x & 1), 18, 26);
    p = Analyse(grey.data());
    Check(p.flat, "the window's own colour, a count apart here and there, is one colour");

    // Noise: every sample its own colour.
    Frame noise = Make();
    uint32_t state = 7;
    for (uint8_t& byte : noise) { state = state * 1664525u + 1013904223u; byte = uint8_t(state >> 24); }
    p = Analyse(noise.data());
    Check(!p.flat && p.isolated > 600, "noise is unlike its neighbours (" + std::to_string(p.isolated) + ")");

    // The player's screenshot: mostly black with coloured blocks one sample across, one in three.
    Frame blocks = Make();
    state = 99;
    for (uint32_t y = 0; y < kHeight; y++)
        for (uint32_t x = 0; x < kWidth; x++)
        {
            state = state * 1664525u + 1013904223u;
            if ((state >> 24) % 3 == 0) Set(blocks, x, y, uint8_t(state >> 16), uint8_t(state >> 8), uint8_t(state));
        }
    p = Analyse(blocks.data());
    Check(!p.flat && p.isolated >= 120, "sparse coloured blocks on black are noise (" + std::to_string(p.isolated) + ")");

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
    Check(!p.flat && p.isolated < 60, "a picture with edges and text is neither (" + std::to_string(p.isolated) + ")");

    // White text on black, a loading screen: not flat, not noise.
    Frame loading = Make();
    for (uint32_t x = 10; x < 50; x += 3) { Set(loading, x, 17, 255, 255, 255); Set(loading, x, 18, 255, 255, 255); }
    p = Analyse(loading.data());
    Check(!p.flat && p.isolated < 60, "a loading screen's text is a picture (" + std::to_string(p.isolated) + ")");

    std::printf("%s\n", g_failures ? "picture probe: FAILED" : "picture probe: ok");
    return g_failures ? 1 : 0;
}
