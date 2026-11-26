#pragma once
// What a device sustains, found by measuring it: the frame rate the run is
// held to and whether the sun casts shadows, where the player has left them
// to the runtime (MW2_ADAPTIVE, on under Android).
//
// The defaults used to be chosen by the chip's name: the two devices the
// project owns are an Adreno 740 and an Adreno 650, and an Adreno 730 or 735
// was handed the 740's without its headroom. A name says little of a phone
// nobody here has -- the same chip throttles in one case and not in another,
// and under another driver is another chip -- so the choice is made from what
// the play in front of the player takes: its frames a second, the graphics
// chip's time a frame by its own clock (gpu_time.h) and the renderer's
// thread's (report.h). What is given up, in order: the sun's shadows, the
// scene's pixels (MW2_SCENE_DYNAMIC, renderer_scene.cpp, which follows the
// chip a sixth of a second at a time), and last the rate. It comes back the
// other way round, slowly, and slower each time it did not hold.
//
// The Governor is the rule alone, with no clock and no device in it
// (tests/adaptive_test.cpp); the functions below it are the runtime's side.
#include <cstdint>
#include <string>
#include <vector>

namespace adaptive
{
    // A window of play, two seconds of it.
    struct Measured
    {
        double seconds = 0;
        double fps = 0;
        // A frame's time on the graphics chip with its clock at its fastest
        // and on the renderer's thread without its waits, in milliseconds;
        // negative where the device does not say.
        double gpuMs = -1, rendererMs = -1;
        // The scene was drawn whole for all of the window, and is at its
        // smallest at the window's end. Both true where it is never drawn
        // smaller: nothing is left to give there and nothing to get back.
        bool sceneWhole = true, sceneAtFloor = true;
    };

    class Governor
    {
    public:
        // `steps`: the rates the run may be held to, the fastest first, each
        // one the display shows evenly; the run starts at the first. With
        // `rate` false the rate is the player's and stays; with `shadows`
        // false the shadows are.
        void Start(std::vector<uint32_t> steps, bool rate, bool shadows);
        // The next window. True when the target or the shadows changed.
        bool Feed(const Measured& window);
        // The scene is about to be drawn smaller because the chip is late:
        // the shadows go first. True when they were on and are now off.
        bool GiveUpShadows();
        // The shadows are the player's from now on, or the runtime's again
        // (which starts without them, as a run does).
        void FollowShadows(bool follow) { m_shadows = follow; m_shadowsOn = false; m_shadowsCalm = 0; }
        // After Start, for a run that had them: they stay until a window
        // says otherwise.
        void KeepShadows() { m_shadowsOn = m_shadows; }

        uint32_t Target() const { return m_steps.empty() ? 0 : m_steps[m_step]; }
        // Whether the sun casts shadows, where that is the runtime's choice.
        bool Shadows() const { return m_shadowsOn; }
        bool RateFollows() const { return m_rate; }
        // Why the last change was made, for the log.
        const std::string& Why() const { return m_why; }

        // A window is a miss under this share of the target.
        static constexpr double kMiss = 0.90;
        // A step down gives up at most this much of what was being drawn:
        // 62 frames a second become 60, 85 do not.
        static constexpr double kGivenUp = 1.12;
        // Seconds of misses before the rate steps down, and before the
        // shadows go: one slow window is a level streaming, not the device.
        static constexpr double kDownSeconds = 10, kShadowsOffSeconds = 4;
        // Seconds of calm before the shadows or the faster rate are tried --
        // thirty, since a level's opening view is its lightest: on an AYN
        // Thor at 120 ten seconds gave Seoul and S.S.D.D. the shadows for
        // the few seconds until their first heavy view took them away -- and
        // the seconds before a change that did not hold is tried again,
        // doubled each time up to the most.
        static constexpr double kShadowsCalm = 30, kUpCalm = 30, kRetry = 60, kMostRetry = 960;
        // The shares of a frame's period the chip and the renderer's thread
        // may take for the shadows to be tried (they cost a fifth to a third
        // of both), and of the faster rate's period for that rate.
        static constexpr double kShadowsGpu = 0.55, kShadowsRenderer = 0.60, kUpShare = 0.70;

    private:
        bool HasRoom(const Measured& window, double periodMs, double gpuShare, double rendererShare) const;
        void StepTo(size_t step, std::string why);

        std::vector<uint32_t> m_steps;
        size_t m_step = 0;
        bool m_rate = false, m_shadows = false, m_shadowsOn = false;
        // Seconds in a row under the target, and what they averaged.
        double m_missed = 0, m_missedFrames = 0;
        // Seconds in a row with room for the shadows, and for the step up.
        double m_shadowsCalm = 0, m_upCalm = 0;
        // Seconds until each may be tried again, and what a failure costs next.
        double m_shadowsWait = 0, m_upWait = 0, m_shadowsRetry = kRetry, m_upRetry = kRetry;
        // Seconds since the shadows came on and since the rate went up: a
        // miss within kRetry of either is that change not holding.
        double m_sinceShadows = kMostRetry, m_sinceUp = kMostRetry;
        std::string m_why;
    };

    // ---- the runtime's side -------------------------------------------------

    // The scene as the renderer draws it this frame (renderer_scene.cpp).
    struct Scene
    {
        bool dynamic = false;
        uint32_t level = 0, levels = 1, percent = 100;
    };

    // Whether anything here runs: MW2_ADAPTIVE, unset on under Android.
    bool On();
    // The renderer finished a frame (its thread).
    void Frame(bool world, const Scene& scene);
    // The sun's shadows as measured, for a player who left them to the
    // runtime (gpu/d3d_hooks.cpp); the reason they are off, or null.
    const char* WhyNoShadows();
    // The scene's stepper is about to draw it smaller; true when the shadows
    // went instead, and the scene waits for what that gives.
    bool GiveUpShadows();
    // The player gave the limit a value in the game's menu (0: the runtime's
    // again), or chose the shadows (`follow` false) or left them to the
    // runtime again.
    void PlayerSetShadows(bool follow);
    void PlayerSetLimit(uint32_t limit);
    // What the app's overlay shows: the rate the run is held to, and the
    // last window's frames a second, chip time and scene.
    struct Now { uint32_t target = 0; bool follows = false; double fps = 0, gpuMs = -1, rendererMs = -1; uint32_t scenePercent = 100; bool shadows = false; };
    Now Current();
    // The performance report a player shares (android/jni.cpp): the device's
    // graphics, the settings in force and how the run went, a line each.
    std::string Report();
    // The same in the log, when the run ends.
    void Write();
}
