// The rule that follows what a device sustains (runtime/adaptive.h), with no
// device: windows of play are fed to it as a phone would have measured them.
//
//   * A device that keeps its rate with room to spare gets the sun's shadows
//     after thirty seconds, and one that is then late loses them after four,
//     and is not given them again for a minute -- two, the second time.
//   * 62 frames a second under 120 become 60; 85 stay under 120, since 60
//     would give up a quarter of them; 25 under 120 go straight to 30.
//   * The rate goes down only once the scene is at its smallest where the
//     chip is what is late, and whatever the scene where it is not.
//   * It comes back up after thirty seconds with room for the faster rate,
//     and a step up that does not hold is tried again after two minutes.
//   * A limit the player chose stays, and so do shadows the player chose.
//   * The display is asked for the rate's own mode where the chip was what
//     was late, and keeps its fastest where the processor was; and it goes
//     back to its fastest, a limit the player chose or not, once the sound
//     has broken up for six seconds in a slower mode.
//   * The app's counter is told what a window's frames waited for: nothing
//     at nine tenths of the rate and above, the graphics chip where it took
//     85% of the frame's period, the processor where it did not.
#include "adaptive.h"

#include <cstdio>
#include <string>

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void Check(bool ok, const std::string& what)
    {
        g_checks++;
        if (ok) return;
        g_failures++;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }

    adaptive::Measured Window(double fps, double gpuMs, double rendererMs, bool whole = true, bool floor = false)
    {
        adaptive::Measured window;
        window.seconds = 2;
        window.fps = fps;
        window.gpuMs = gpuMs;
        window.rendererMs = rendererMs;
        window.sceneWhole = whole;
        window.sceneAtFloor = floor;
        return window;
    }

    // Feeds the same window for `seconds`; true when anything changed.
    bool Run(adaptive::Governor& governor, const adaptive::Measured& window, double seconds)
    {
        bool changed = false;
        for (double at = 0; at < seconds; at += window.seconds) changed |= governor.Feed(window);
        return changed;
    }
}

int main()
{
    using adaptive::Governor;

    // ---- the sun's shadows ---------------------------------------------------
    {
        Governor governor;
        governor.Start({ 60, 30 }, true, true);
        Check(!governor.Shadows(), "a run starts without the shadows");
        Check(!Run(governor, Window(60, 6.0, 5.0), 28) && !governor.Shadows(), "28 seconds of room are not thirty");
        Check(governor.Feed(Window(60, 6.0, 5.0)) && governor.Shadows(), "thirty seconds of room give the shadows");
        Check(governor.Target() == 60, "and the rate is as it was");
        Check(!governor.Feed(Window(45, 15.0, 5.0)) && governor.Shadows(), "one late window keeps them");
        Check(governor.Feed(Window(45, 15.0, 5.0)) && !governor.Shadows(), "four late seconds take them");
        Check(!Run(governor, Window(60, 6.0, 5.0), 58) && !governor.Shadows(), "not again within a minute");
        Check(Run(governor, Window(60, 6.0, 5.0), 34) && governor.Shadows(), "again after it, with thirty seconds of room");
        Run(governor, Window(45, 15.0, 5.0), 4);
        Check(!governor.Shadows(), "late again, off again");
        Check(!Run(governor, Window(60, 6.0, 5.0), 118) && !governor.Shadows(), "the second time waits two minutes");
        Check(Run(governor, Window(60, 6.0, 5.0), 34) && governor.Shadows(), "and then gives them");
    }
    {
        Governor governor;
        governor.Start({ 60 }, false, true);
        Check(!Run(governor, Window(60, 12.0, 5.0), 60) && !governor.Shadows(),
              "a chip at 12 of 16.7 ms has no room for the shadows");
        Check(!Run(governor, Window(60, 6.0, 12.0), 60) && !governor.Shadows(),
              "nor a renderer's thread at 12 of 16.7 ms");
        Check(!Run(governor, Window(60, 6.0, 5.0, false), 60) && !governor.Shadows(),
              "nor a scene drawn smaller in the window");
        Check(Run(governor, Window(60, -1, -1), 30) && governor.Shadows(),
              "a device that gives no times is given them on its frames alone");
    }
    {
        Governor governor;
        governor.Start({ 120, 60 }, true, true);
        Run(governor, Window(120, 3.0, 3.0), 30);
        Check(governor.Shadows(), "at 120 with 3 ms of each: the shadows");
        Check(governor.GiveUpShadows() && !governor.Shadows(), "the scene's stepper takes them first");
        Check(!governor.GiveUpShadows(), "and has nothing to take the second time");
    }
    {
        Governor governor;
        governor.Start({ 60 }, false, false);
        Check(!Run(governor, Window(60, 3.0, 3.0), 60) && !governor.Shadows(), "the player's shadows are not the rule's");
        Check(!governor.GiveUpShadows(), "nor the scene's stepper's");
    }

    // ---- the rate down ---------------------------------------------------------
    {
        Governor governor;
        governor.Start({ 120, 60, 30 }, true, true);
        Check(!Run(governor, Window(62, 15.0, 4.0, false, true), 8), "eight seconds at 62 under 120 are not ten");
        Check(governor.Feed(Window(62, 15.0, 4.0, false, true)) && governor.Target() == 60, "ten are: held to 60");
    }
    {
        Governor governor;
        governor.Start({ 120, 60, 30 }, true, true);
        Check(!Run(governor, Window(85, 11.0, 4.0, false, true), 120) && governor.Target() == 120,
              "85 under 120 stay: 60 would give up a quarter of them");
    }
    {
        Governor governor;
        governor.Start({ 120, 90, 60, 45, 30 }, true, true);
        Run(governor, Window(100, 9.5, 4.0, false, true), 10);
        Check(governor.Target() == 90, "100 under 120 on a screen with a 90 Hz mode: 90");
    }
    {
        Governor governor;
        governor.Start({ 120, 60, 30 }, true, true);
        Run(governor, Window(25, 38.0, 6.0, false, true), 10);
        Check(governor.Target() == 30, "25 under 120 go straight to 30");
        Check(!Run(governor, Window(25, 38.0, 6.0, false, true), 60) && governor.Target() == 30,
              "and there is nothing under the last step");
    }
    {
        Governor governor;
        governor.Start({ 120, 60, 30 }, true, true);
        Check(!Run(governor, Window(62, 15.0, 4.0, false, false), 60) && governor.Target() == 120,
              "the chip late and the scene not yet at its smallest: the pixels go first");
        Check(Run(governor, Window(62, 4.0, 15.0, false, false), 10) && governor.Target() == 60,
              "the chip idle and the frames late: the scene would buy nothing, the rate goes");
    }
    {
        Governor governor;
        governor.Start({ 120, 60, 30 }, true, true);
        Run(governor, Window(62, 15.0, 4.0, false, true), 6);
        Run(governor, Window(119, 7.0, 4.0), 2);
        Check(!Run(governor, Window(62, 15.0, 4.0, false, true), 8) && governor.Target() == 120,
              "the ten seconds are in a row");
    }

    // ---- and up again ----------------------------------------------------------
    {
        Governor governor;
        governor.Start({ 120, 60, 30 }, true, false);
        Run(governor, Window(62, 15.0, 4.0, false, true), 10);
        Check(governor.Target() == 60, "down to 60");
        Check(!Run(governor, Window(60, 7.0, 4.0), 120) && governor.Target() == 60,
              "7 ms of chip is no room for 120 (8.3 ms a frame)");
        // The minute after the step down has passed in the lines above.
        Check(!Run(governor, Window(60, 5.0, 4.0), 28) && governor.Target() == 60, "28 seconds of room are not 30");
        Check(governor.Feed(Window(60, 5.0, 4.0)) && governor.Target() == 120, "30 are: back to 120");
        Run(governor, Window(62, 15.0, 4.0, false, true), 10);
        Check(governor.Target() == 60, "which did not hold");
        Check(!Run(governor, Window(60, 5.0, 4.0), 118) && governor.Target() == 60, "so the next try waits two minutes");
        Check(Run(governor, Window(60, 5.0, 4.0), 32) && governor.Target() == 120, "and comes");
    }
    {
        Governor governor;
        governor.Start({ 120, 60 }, true, true);
        Run(governor, Window(62, 15.0, 4.0, false, true), 10);
        Run(governor, Window(60, 5.0, 4.0), 90);
        Check(governor.Target() == 120 && !governor.Shadows(), "room for both: the faster rate before the shadows");
    }

    // ---- the player's limit ------------------------------------------------------
    {
        Governor governor;
        governor.Start({ 120 }, false, true);
        Check(!Run(governor, Window(25, 38.0, 6.0, false, true), 120) && governor.Target() == 120,
              "a limit the player chose stays");
        Check(!governor.RateFollows(), "and says it does not follow");
    }

    // ---- the display's mode ------------------------------------------------------
    {
        Governor governor;
        governor.Start({ 120, 60, 30 }, true, false);
        Check(governor.Display() == 120 && !governor.DisplayKept(), "a run starts in the display's fastest mode");
        Run(governor, Window(62, 15.0, 4.0, false, true), 10);
        Check(governor.Target() == 60 && governor.Display() == 60 && !governor.DisplayKept(),
              "the chip late: the display follows the rate down");
        Run(governor, Window(60, 5.0, 4.0), 90);
        Check(governor.Target() == 120 && governor.Display() == 120, "and up again");
    }
    {
        Governor governor;
        governor.Start({ 120, 60, 30 }, true, false);
        Run(governor, Window(46, 4.0, 10.0), 10);
        Check(governor.Target() == 60 && governor.Display() == 120 && governor.DisplayKept(),
              "the processor late: held to 60, the display kept at 120");
        Run(governor, Window(25, 14.5, 20.0, false, true), 10);
        Check(governor.Target() == 30 && governor.Display() == 120,
              "the chip late after it: the display stays where the processor put it");
        Run(governor, Window(30, 5.0, 4.0), 90);
        Check(governor.Target() == 60 && governor.Display() == 120, "a step up, not yet the first: still kept");
        Run(governor, Window(60, 5.0, 4.0), 30);
        Check(governor.Target() == 120 && !governor.DisplayKept(), "back at the first step");
        Run(governor, Window(62, 15.0, 4.0, false, true), 10);
        Check(governor.Target() == 60 && governor.Display() == 60, "and the next step down is judged afresh");
    }
    {
        Governor governor;
        governor.Start({ 30 }, false, false, 120);
        Check(governor.Target() == 30 && governor.Display() == 30, "a limit the player chose: the display follows it");
        Check(!governor.Sound(2, 3), "three underruns in two seconds are not broken sound");
        Check(!governor.Sound(2, 60) && !governor.Sound(2, 60), "four seconds of it are not six");
        Check(!governor.Sound(2, 0) && !governor.Sound(2, 60) && !governor.Sound(2, 60), "the six seconds are in a row");
        Check(governor.Sound(2, 60) && governor.Display() == 120 && governor.Target() == 30,
              "six seconds of broken sound: the display goes back to its fastest mode, the limit stays");
        Check(!governor.Sound(2, 60), "once");
        governor.Start({ 60 }, false, false, 120);
        Check(governor.Display() == 120 && governor.DisplayKept(), "and another limit does not take the display down again");
        governor.Start({ 120, 60, 30 }, true, false, 120);
        Run(governor, Window(62, 15.0, 4.0, false, true), 10);
        Check(governor.Target() == 60 && governor.Display() == 120, "nor does the chip late");
    }
    {
        Governor governor;
        governor.Start({ 120, 60, 30 }, true, false);
        Run(governor, Window(62, 15.0, 4.0, false, true), 10);
        Run(governor, Window(60, 12.0, 4.0), 2);
        Check(governor.Display() == 60, "held to 60 by the chip, the display at 60");
        governor.Sound(2, 60);
        governor.Sound(2, 60);
        Check(governor.Sound(2, 60) && governor.Display() == 120 && governor.Target() == 60,
              "the sound breaks up there: the display goes back, the rate stays held");
    }
    {
        Governor governor;
        governor.Start({ 30, 0, 120, 60, 60 }, true, true);
        Check(governor.Target() == 120, "the steps start at the fastest, in any order given");
        Governor none;
        none.Start({}, true, true);
        Check(none.Target() == 0 && !none.Feed(Window(10, 1, 1)), "no steps: no target, nothing to decide");
    }
    {
        using adaptive::Late;
        Check(Governor::WhatWasLate(Window(59, 15.5, 6.0), 60) == Late::Nothing, "59 under 60 waits for nothing");
        Check(Governor::WhatWasLate(Window(54, 12.0, 6.0), 60) == Late::Nothing, "nor does nine tenths of the rate");
        Check(Governor::WhatWasLate(Window(48, 15.0, 6.0), 60) == Late::Chip, "48 under 60 with the chip at 15 ms: the chip");
        Check(Governor::WhatWasLate(Window(48, 9.0, 14.0), 60) == Late::Processor, "with the chip at 9 ms: the processor");
        Check(Governor::WhatWasLate(Window(90, 7.5, 3.0), 120) == Late::Chip, "90 under 120 with the chip at 7.5 ms: the chip");
        Check(Governor::WhatWasLate(Window(48, -1, 14.0), 60) == Late::Unknown, "a chip that keeps no time is not named");
        Check(Governor::WhatWasLate(Window(45, 20.0, 5.0), 0) == Late::Chip, "no limit: the chip at 20 ms of a 22 ms frame");
        Check(Governor::WhatWasLate(Window(45, 8.0, 5.0), 0) == Late::Processor, "no limit: the chip at 8 ms of 22");
        Check(Governor::WhatWasLate(Window(0, 8.0, 5.0), 60) == Late::Unknown, "no frames: nothing to say");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
