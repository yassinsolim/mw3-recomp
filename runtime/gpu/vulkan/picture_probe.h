#pragma once
// What the first frames of a run look like, in a few numbers.
//
// A driver can load, make every pipeline and present sixty frames a second
// of nothing: one colour, or speckle (a player's Snapdragon 8 Gen 2,
// November 2026). The frame counter cannot tell, so until the app has seen
// a picture on the driver the presenter copies a frame twice a second, 64 by
// 36 samples of it, and says what it saw; the app decides over time
// (FrameWatch.kt) and offers the next driver. After that, and on every later
// run on a driver that has shown a picture, nothing is copied.
//
// No one number tells a broken frame from a busy one: frames of play have
// up to 300 samples in a thousand unlike all four neighbours (foliage,
// gravel, a chain-link fence), the player's screenshot of speckle 158. What
// the screenshot has that 19,006 looks at 102 recordings of the five games
// have 12 times is all three of: speckle, on a ground of one colour, that is
// somewhere else half a second later (docs/android.md has the measurements;
// tools/frame_watch_calibrate.py makes them).
#include <cstdint>
#include <cstdlib>

namespace vk::probe
{
    constexpr uint32_t kWidth = 64, kHeight = 36;
    constexpr uint32_t kSamples = kWidth * kHeight;

    struct Picture
    {
        uint32_t serial = 0;     // how many frames have been looked at; 0: none yet
        bool flat = false;       // every sample is one colour
        uint32_t isolated = 0;   // per thousand: samples unlike all four of their neighbours
        uint32_t ground = 0;     // per thousand: samples of the frame's middle colour
        uint32_t stable = 0;     // per thousand: isolated samples that were so, and the same colour, at the look before
        uint32_t luma = 0;       // the mean brightness, 0 to 255
        uint32_t draws = 0;      // draws in the newest frame the renderer finished, 4095 at the most
    };

    // The numbers of kWidth x kHeight samples of four bytes, red first, and of
    // the look before them (`before`, null at the first: everything isolated
    // then counts as stable, which is the answer that accuses nothing).
    inline Picture Analyse(const uint8_t* rgba, const uint8_t* before = nullptr)
    {
        constexpr int kFlat = 6, kUnlike = 24;
        auto unlike = [](const uint8_t* p, const uint8_t* q, int by) {
            return std::abs(p[0] - q[0]) > by || std::abs(p[1] - q[1]) > by || std::abs(p[2] - q[2]) > by;
        };
        auto isolated = [&](const uint8_t* of, uint32_t at) {
            const uint8_t* p = of + at * 4;
            return unlike(p, p - 4, kUnlike) && unlike(p, p + 4, kUnlike) &&
                   unlike(p, p - kWidth * 4, kUnlike) && unlike(p, p + kWidth * 4, kUnlike);
        };
        // The middle value of each channel: the colour of the ground wherever
        // more than half the frame is it, and near enough it down to a third.
        uint8_t middle[3] = {};
        for (int c = 0; c < 3; c++)
        {
            uint32_t count[256] = {};
            for (uint32_t i = 0; i < kSamples; i++) count[rgba[i * 4 + c]]++;
            uint32_t below = 0;
            for (int value = 0; value < 256; value++)
            {
                below += count[value];
                if (below > kSamples / 2) { middle[c] = uint8_t(value); break; }
            }
        }
        Picture picture;
        picture.flat = true;
        uint64_t sum = 0;
        uint32_t alone = 0, same = 0, ground = 0;
        for (uint32_t y = 0; y < kHeight; y++)
        {
            for (uint32_t x = 0; x < kWidth; x++)
            {
                const uint32_t at = y * kWidth + x;
                const uint8_t* p = rgba + at * 4;
                sum += p[0] + p[1] + p[2];
                if (unlike(p, rgba, kFlat)) picture.flat = false;
                if (!unlike(p, middle, kFlat)) ground++;
                if (x == 0 || y == 0 || x == kWidth - 1 || y == kHeight - 1) continue;
                if (!isolated(rgba, at)) continue;
                alone++;
                if (!before || (isolated(before, at) && !unlike(p, before + at * 4, kUnlike))) same++;
            }
        }
        constexpr uint32_t kInner = (kWidth - 2) * (kHeight - 2);
        picture.isolated = alone * 1000 / kInner;
        picture.stable = same * 1000 / kInner;
        picture.ground = ground * 1000 / kSamples;
        picture.luma = uint32_t(sum / (3ull * kSamples));
        return picture;
    }

#ifdef MW2_HAVE_VULKAN
    // The presenter's two calls, on its thread. Record: `commands` is the
    // frame slot's buffer being recorded, `frame` the renderer's image in
    // TRANSFER_SRC layout, left so; it copies only when a look is due.
    // Collect: the slot's fence has signalled.
    void Record(void* device, void* physical, void* commands, uint32_t slot,
                void* frame, uint32_t width, uint32_t height);
    void Collect(uint32_t slot);
    void Destroy();
#endif
    // The renderer's: how many draws the frame it has just finished had. A
    // screen of one colour that nothing was drawn on is a loading screen.
    void Drawn(uint64_t draws);
    // The app has made up its mind (or knows the driver from an earlier
    // run): nothing more is copied in this run. Any thread.
    void Settle();
    // The last frame looked at; any thread.
    Picture Last();
}
