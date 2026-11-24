#pragma once
// MW2_FPS_LIMIT: how many frames a second the title draws at most.
//
// Unset or 60 is the console's pacing: D3D retires a frame at the guest's
// blank and the title's own limiter (com_maxfps) is at its 60 (Advanced
// Warfare on a 60 Hz display excepted: Mode). 30 is the
// console's pacing too, a frame held for two of the guest's blanks, which is
// D3D's own half-rate interval (d3d_hooks.cpp): every frame is as long as the
// last, where a limiter counting 33 ms against blanks 33.3 ms apart shows a
// frame for one blank or three every second or so. Any other number takes
// the title off the blank the way the console's D3D offers it, and what
// paces it then depends on the display:
//
//   * the number is the display's refresh rate or above it (120 on a 120 Hz
//     panel, and 120 on one the system holds at 60): the display paces it.
//     The title's limiter is off, every frame is shown, and the renderer
//     waits for the window to have taken a frame before it finishes the
//     next, so there is one frame a blank whatever the panel's real rate is
//     -- a "120 Hz" panel may run at 118.4.
//   * a number under it: the title's limiter paces it, with that number as
//     com_maxfps; 0 is no limit. The limiter counts whole milliseconds a
//     frame, so the rate it keeps is 1000 over a whole number: 45 gives 45.5.
//     The window shows the newest frame at each blank.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include "env.h"

namespace frame_rate
{
    constexpr uint32_t kConsole = 60;

    // com_maxfps: 0 is no limit, and 1000 is the most the title takes.
    inline uint32_t Limit()
    {
        static const uint32_t limit = uint32_t(std::min<uint64_t>(env::Number("MW2_FPS_LIMIT", kConsole), 1000));
        return limit;
    }

    // How many of the guest's 60 blanks a frame is held for where the
    // console's own pacing keeps the limit: one at 60, two at 30. 0 where the
    // limit is not the console's to keep.
    inline uint32_t ConsoleBlanks()
    {
        const uint32_t limit = Limit();
        return limit == kConsole ? 1 : limit * 2 == kConsole ? 2 : 0;
    }

    // Frames are retired at the guest's blank, as on the console.
    inline bool Console() { return ConsoleBlanks() != 0; }

    // The display the window is on, as the presenter knows it: the rate its
    // mode gives, which the pacing is chosen by, and its blank's period, which
    // is that rate's until the presents reaching the screen have measured it.
    // Both are 0 with no window.
    namespace detail
    {
        inline std::atomic<double> g_displayHz{ 0.0 }, g_displayPeriodNs{ 0.0 };
    }
    inline void SetDisplay(double hz, double periodNs)
    {
        detail::g_displayHz.store(hz, std::memory_order_relaxed);
        detail::g_displayPeriodNs.store(periodNs, std::memory_order_relaxed);
    }
    inline double DisplayHz() { return detail::g_displayHz.load(std::memory_order_relaxed); }
    inline double DisplayPeriodNs() { return detail::g_displayPeriodNs.load(std::memory_order_relaxed); }

    enum Pacing { kByConsole, kByDisplay, kByLimiter };

    // A mode that calls itself 120 may be 119.88 or 118.4: 3% takes them all
    // and leaves 90 against 120 or 144 well apart. The mode's rate decides,
    // not the measured period, which moves a little as it is measured and
    // must not move the title between two pacings. A limit above the
    // display's rate is the display's too: an AYN Thor whose system setting
    // holds the panel at 60 was drawn 125 frames a second under the limit of
    // 120 it starts at, to show 60 of them at uneven steps of the game's time.
    //
    // Advanced Warfare at the console's 60 is the display's as well, where
    // the display runs at 60: retired at the guest's blank, a fifth of this
    // title's frames come a millisecond or more after they are due, whatever
    // the GPU has left (a multiplayer match at 6.5 ms of GPU a frame as much
    // as the campaign at 13). Seoul's drop pod on a Retroid Pocket Flip 2:
    // 79% of the frames within 1.05 periods of the one before under the
    // console's pacing, 95% under the display's, 59.6 a second either way.
    // The half rate stays the console's, and so does 60 on a faster display,
    // which would show every frame the title can make.
    inline Pacing Mode()
    {
        const double hz = DisplayHz();
        const uint32_t limit = Limit();
#if defined(AW_TITLE)
        if (limit == kConsole && hz > 0 && double(limit) >= 0.97 * hz && double(limit) <= 1.03 * hz)
            return kByDisplay;
#endif
        if (Console()) return kByConsole;
        return limit && hz > 0 && double(limit) >= 0.97 * hz ? kByDisplay : kByLimiter;
    }

    // How many of the display's blanks a frame is due to stay on screen: one,
    // or more where the limit is a fraction of the display's rate (60 on a
    // 120 Hz panel is two).
    inline uint32_t BlanksAFrame()
    {
        const double hz = DisplayHz();
        const uint32_t limit = Limit();
        if (!limit || hz <= 0) return 1;
        return uint32_t(std::max(1L, std::lround(hz / double(limit))));
    }
}
