#pragma once
// What the first frames of a run look like, in three numbers.
//
// A driver can load, make every pipeline and present sixty frames a second
// of nothing: one colour, or blocks of noise (a player's Snapdragon 8 Gen 2,
// November 2026). The frame counter cannot tell, so for the first minutes of
// a run the presenter copies a frame twice a second, 64 by 36 samples of it,
// and says what it saw; the app decides over time (FrameWatch.kt) and offers
// the next driver. After that nothing is copied.
#include <cstdint>
#include <cstdlib>

namespace vk::probe
{
    constexpr uint32_t kWidth = 64, kHeight = 36;

    struct Picture
    {
        uint32_t serial = 0;     // how many frames have been looked at; 0: none yet
        bool flat = false;       // every sample is one colour
        uint32_t isolated = 0;   // per thousand: samples unlike all four of their neighbours
        uint32_t luma = 0;       // the mean brightness, 0 to 255
    };

    // The three numbers of kWidth x kHeight samples of four bytes, red first.
    // A picture's neighbouring samples mostly agree, across an edge on one
    // side at least; noise agrees with nothing around it.
    inline Picture Analyse(const uint8_t* rgba)
    {
        constexpr int kFlat = 6, kUnlike = 24;
        auto unlike = [&](uint32_t a, uint32_t b, int by) {
            const uint8_t* p = rgba + a * 4;
            const uint8_t* q = rgba + b * 4;
            return std::abs(p[0] - q[0]) > by || std::abs(p[1] - q[1]) > by || std::abs(p[2] - q[2]) > by;
        };
        Picture picture;
        picture.flat = true;
        uint64_t sum = 0;
        uint32_t isolated = 0;
        for (uint32_t y = 0; y < kHeight; y++)
        {
            for (uint32_t x = 0; x < kWidth; x++)
            {
                const uint32_t at = y * kWidth + x;
                const uint8_t* p = rgba + at * 4;
                sum += p[0] + p[1] + p[2];
                if (unlike(at, 0, kFlat)) picture.flat = false;
                if (x == 0 || y == 0 || x == kWidth - 1 || y == kHeight - 1) continue;
                if (unlike(at, at - 1, kUnlike) && unlike(at, at + 1, kUnlike) &&
                    unlike(at, at - kWidth, kUnlike) && unlike(at, at + kWidth, kUnlike))
                    isolated++;
            }
        }
        picture.isolated = isolated * 1000 / ((kWidth - 2) * (kHeight - 2));
        picture.luma = uint32_t(sum / (3ull * kWidth * kHeight));
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
    // The last frame looked at; any thread.
    Picture Last();
}
