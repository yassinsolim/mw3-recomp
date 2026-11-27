#include "adaptive.h"
#include "env.h"
#include "frame_rate.h"
#include "log.h"
#include "report.h"
#ifdef MW2_HAVE_VULKAN
#include "gpu/vulkan/gpu_time.h"
#endif
#include "gpu/vulkan/pipeline.h"
#ifdef MW2_ANDROID
#include "android/android.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>

// The runtime's side of adaptive.h; the rule itself is adaptive_governor.cpp.

#ifdef MW2_ANDROID
namespace android::detail { void VoteAgain(); }
#endif

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr double kWindowSeconds = 2.0;
    // A window says something of the device when this much of it was play.
    constexpr double kPlayShare = 0.8;
    // The chip's clock is read this often, in frames: a file of the kernel's.
    constexpr uint32_t kClockEvery = 16;
    // Windows kept for the report: ten minutes.
    constexpr size_t kKept = 300;
    // A line of the log this often, in seconds of play: a run a player
    // ends is killed, and says nothing at its end.
    constexpr double kSayEvery = 60.0;

    struct Kept
    {
        double at = 0, fps = 0, gpuMs = -1, rendererMs = -1;
        uint32_t target = 0, scenePercent = 100;
        bool shadows = false;
    };

    struct Change { double at = 0; std::string what; };

    // The renderer's thread alone, between windows.
    struct Window
    {
        bool started = false, lastWasWorld = false;
        Clock::time_point first, began, last;
        uint64_t playNs = 0;
        uint32_t intervals = 0, frames = 0, sinceClock = 0;
        uint64_t busyAtClock = 0, spentAt[report::kTimeCount]{};
        double busyAtFullNs = 0;
        uint32_t deepest = 0;
        uint64_t underrunsAt = 0;
        // What each frame of play in the window took, in milliseconds, for
        // the run's sum (adaptive::Tally): twice the fastest display's.
        float frameMs[640];
        uint32_t timed = 0;
    } w;

    std::mutex g_lock;
    adaptive::Governor g_governor;          // under g_lock
    adaptive::Tally g_tally;                // under g_lock
    std::deque<Kept> g_kept;                // under g_lock
    std::vector<Change> g_changes;          // under g_lock
    std::vector<uint32_t> g_steps;          // under g_lock: the rates the display shows evenly
    double g_playSeconds = 0, g_frames = 0, g_missedSeconds = 0, g_slowest = 0, g_said = 0;   // under g_lock
    bool g_startedLocked = false;           // under g_lock
    Clock::time_point g_keptAt;             // under g_lock: when the newest of g_kept ended
    // Under g_lock: the seconds of play whose frames waited for the chip and for the processor.
    double g_chipSeconds = 0, g_processorSeconds = 0;
    // Read by the title's thread at every present, and by the app's.
    std::atomic<bool> g_shadows{ false }, g_shadowsFollow{ true };
    std::atomic<uint32_t> g_scenePercent{ 100 };
    // The rate the display is asked for (Governor::Display); 0 before the
    // first frame. Read by the surface's vote and the app.
    std::atomic<uint32_t> g_display{ 0 };

    // Under g_lock: the rate of the display's fastest mode.
    uint32_t Fastest() { return g_steps.empty() ? 0 : *std::max_element(g_steps.begin(), g_steps.end()); }

    // Under g_lock: which way the display went, for the log and the report
    // (Governor::Display), after "the display ".
    std::string DisplaySays()
    {
        const char* kept = g_governor.DisplayKept();
        if (!kept) return "follows the rate";
        return "is kept at " + std::to_string(g_governor.Fastest()) + " Hz because " + kept;
    }

    std::vector<uint32_t> Steps()
    {
        // MW2_FPS_STEPS, from the app, which knows the display's modes:
        // "120,60,30". Without it the limit and its halves down to 30.
        std::vector<uint32_t> steps;
        if (const char* text = env::Text("MW2_FPS_STEPS"))
        {
            for (const char* at = text; *at;)
            {
                char* end = nullptr;
                const unsigned long rate = std::strtoul(at, &end, 10);
                if (end == at) break;
                if (rate >= 20 && rate <= 1000) steps.push_back(uint32_t(rate));
                at = *end ? end + 1 : end;
            }
        }
        if (steps.empty())
            for (uint32_t rate = frame_rate::Limit(); rate >= 30; rate /= 2) steps.push_back(rate);
        return steps;
    }

    // Under g_lock.
    void StartLocked()
    {
        if (g_startedLocked) return;
        g_startedLocked = true;
        g_steps = Steps();
        const bool rate = env::Flag("MW2_FPS_AUTO");
        g_shadowsFollow.store(!env::Text("MW2_SHADOWS"), std::memory_order_relaxed);
        g_governor.Start(rate ? g_steps : std::vector<uint32_t>{ frame_rate::Limit() }, rate,
                         g_shadowsFollow.load(std::memory_order_relaxed), Fastest());
        g_display.store(g_governor.Display(), std::memory_order_relaxed);
        std::string list;
        for (const uint32_t step : g_steps) list += (list.empty() ? "" : ", ") + std::to_string(step);
        LOGI("adaptive: the run follows what this device sustains: the frame rate %s, the sun's shadows %s;"
             " MW2_ADAPTIVE=0 for neither",
             rate ? ("starts at " + std::to_string(g_governor.Target()) + " and may go down to one of " + list).c_str()
                  : "is the player's limit and stays",
             g_shadowsFollow.load(std::memory_order_relaxed) ? "come on once a window of play has room for them"
                                                             : "are the player's choice");
    }

    // The surface votes for the display rate that goes with what the run is
    // held to (adaptive::DisplayRate).
    void Vote()
    {
#ifdef MW2_ANDROID
        android::detail::VoteAgain();
#endif
    }

    void Apply(uint32_t target)
    {
        frame_rate::SetLimit(target);
        Vote();
    }

    // How often the sound's output has had nothing to play so far.
    uint64_t Underruns()
    {
#ifdef MW2_ANDROID
        return android::audio::Underruns();
#else
        return 0;
#endif
    }

    // The chip's time at work so far, by its own clock; 0 where it keeps
    // none, and in a build without the renderer.
    uint64_t ChipBusy()
    {
#ifdef MW2_HAVE_VULKAN
        return vk::gpu_time::Busy();
#else
        return 0;
#endif
    }

    double ClockShare()
    {
#ifdef MW2_ANDROID
        return android::perf::AdrenoClockShare();
#else
        return 1.0;
#endif
    }

    std::string Summary()   // under g_lock
    {
        char text[440];
        const double fps = g_playSeconds > 0 ? g_frames / g_playSeconds : 0;
        double gpu = 0, renderer = 0;
        size_t gpus = 0, renderers = 0;
        for (const Kept& kept : g_kept)
        {
            if (kept.gpuMs >= 0) { gpu += kept.gpuMs; gpus++; }
            if (kept.rendererMs >= 0) { renderer += kept.rendererMs; renderers++; }
        }
        int length = std::snprintf(text, sizeof text,
                                   "%.0f s of play at %.1f frames a second, the slowest two seconds %.1f, %.0f%% of"
                                   " the time under nine tenths of the rate it was held to",
                                   g_playSeconds, fps, g_slowest,
                                   g_playSeconds > 0 ? 100.0 * g_missedSeconds / g_playSeconds : 0.0);
        if (gpus)
            length += std::snprintf(text + length, sizeof text - size_t(length), "; the graphics chip %.1f ms a frame",
                                    gpu / double(gpus));
        if (renderers)
            length += std::snprintf(text + length, sizeof text - size_t(length), ", the renderer's thread %.1f ms",
                                    renderer / double(renderers));
        // Which of the two the late frames waited for, as the app's counter says it.
        if (g_chipSeconds + g_processorSeconds > 0)
            std::snprintf(text + length, sizeof text - size_t(length),
                          "; the frames waited for the graphics chip %.0f%% of the time and for the processor %.0f%%",
                          100.0 * g_chipSeconds / g_playSeconds, 100.0 * g_processorSeconds / g_playSeconds);
        return text;
    }
}

bool adaptive::On()
{
#ifdef MW2_ANDROID
    static const bool on = !env::Text("MW2_ADAPTIVE") || env::Flag("MW2_ADAPTIVE");
#else
    static const bool on = env::Flag("MW2_ADAPTIVE");
#endif
    return on;
}

void adaptive::Frame(bool world, const Scene& scene)
{
    if (!On()) return;
    const auto now = Clock::now();
    g_scenePercent.store(scene.percent, std::memory_order_relaxed);
    if (!w.started)
    {
        w.started = true;
        w.first = w.began = w.last = now;
        w.busyAtClock = ChipBusy();
        w.underrunsAt = Underruns();
        for (size_t time = 0; time < report::kTimeCount; time++) w.spentAt[time] = report::SpentSoFar(report::Time(time));
        report::KeepTimes();
        std::lock_guard lock(g_lock);
        StartLocked();
        return;
    }
    // Play is one world frame after another: a menu, a loading screen or a
    // movie between two of them says nothing of what the device sustains.
    if (world && w.lastWasWorld)
    {
        const auto took = now - w.last;
        w.playNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(took).count());
        w.intervals++;
        if (w.timed < std::size(w.frameMs)) w.frameMs[w.timed++] = std::chrono::duration<float, std::milli>(took).count();
    }
    w.lastWasWorld = world;
    w.last = now;
    w.frames++;
    w.deepest = std::max(w.deepest, scene.dynamic ? scene.level : 0u);
    if (++w.sinceClock >= kClockEvery)
    {
        // What the chip took at the clock it ran at, as what it would take
        // at its fastest: the governor holds the clock down while there is
        // time in hand, and a frame that is long for that has room all the same.
        const uint64_t busy = ChipBusy();
        w.busyAtFullNs += double(busy - w.busyAtClock) * ClockShare();
        w.busyAtClock = busy;
        w.sinceClock = 0;
    }
    const double seconds = std::chrono::duration<double>(now - w.began).count();
    if (seconds < kWindowSeconds) return;

    const uint64_t busy = ChipBusy();
    w.busyAtFullNs += double(busy - w.busyAtClock) * ClockShare();
    uint64_t spent[report::kTimeCount];
    for (size_t time = 0; time < report::kTimeCount; time++) spent[time] = report::SpentSoFar(report::Time(time));
    auto took = [&](report::Time time) { return double(spent[time] - w.spentAt[time]); };

    Measured window;
    window.seconds = seconds;
    const double playSeconds = double(w.playNs) / 1e9;
    const bool play = w.intervals > 0 && playSeconds >= kPlayShare * seconds;
    if (play)
    {
        window.fps = double(w.intervals) / playSeconds;
        if (busy) window.gpuMs = w.busyAtFullNs / 1e6 / double(w.frames);
        // Its work alone: waiting for the chip, the display or the recorder
        // is time another rate would not need.
        window.rendererMs = std::max(0.0, took(report::kExecuting) - took(report::kGpuWait) -
                                              took(report::kDisplayWait) - took(report::kRecorderWait)) /
                            1e6 / double(w.frames);
        window.sceneWhole = !scene.dynamic || w.deepest == 0;
        window.sceneAtFloor = !scene.dynamic || scene.level + 1 >= scene.levels;
    }
    w.began = now;
    w.playNs = 0;
    w.intervals = w.frames = w.sinceClock = 0;
    w.busyAtClock = busy;
    w.busyAtFullNs = 0;
    w.deepest = scene.dynamic ? scene.level : 0;
    std::copy(std::begin(spent), std::end(spent), w.spentAt);
    const uint64_t underruns = Underruns();
    const uint32_t heard = uint32_t(underruns - w.underrunsAt);
    w.underrunsAt = underruns;

    uint32_t apply = 0;
    bool vote = false;
    {
        std::lock_guard lock(g_lock);
        const uint32_t target = g_governor.Target(), display = g_governor.Display();
        const bool shadows = g_governor.Shadows();
        const double at = std::chrono::duration<double>(now - w.first).count();
        // The sound is listened to in every window, a menu's and a movie's
        // too, while the display runs in a slower mode because it was asked to.
        const double hz = frame_rate::DisplayHz();
        const bool slower = display < g_governor.Fastest() && hz > 0 && hz < 0.97 * double(g_governor.Fastest());
        if (g_governor.Sound(seconds, slower ? heard : 0))
        {
            char what[160];
            std::snprintf(what, sizeof what, "the display %s (its %.0f Hz mode: %u underruns in the last %.0f s)",
                          DisplaySays().c_str(), hz, heard, seconds);
            LOGI("adaptive: %s", what);
            g_changes.push_back({ at, what });
        }
        if (play)
        {
            g_playSeconds += seconds;
            g_frames += window.fps * seconds;
            if (target && window.fps < Governor::kMiss * double(target)) g_missedSeconds += seconds;
            const Late late = Governor::WhatWasLate(window, target);
            if (late == Late::Chip) g_chipSeconds += seconds;
            else if (late == Late::Processor) g_processorSeconds += seconds;
            g_slowest = g_slowest > 0 ? std::min(g_slowest, window.fps) : window.fps;
            g_kept.push_back({ at, window.fps, window.gpuMs, window.rendererMs, target, scene.percent, shadows });
            g_keptAt = now;
            if (g_kept.size() > kKept) g_kept.pop_front();
            for (uint32_t frame = 0; frame < w.timed; frame++) g_tally.Frame(w.frameMs[frame]);
            g_tally.Window(window);
        }
        w.timed = 0;
        if (play && g_governor.Feed(window))
        {
            char what[320];
            if (g_governor.Target() != target)
            {
                std::snprintf(what, sizeof what, "held to %u frames a second, not %u: %s; the display %s",
                              g_governor.Target(), target, g_governor.Why().c_str(), DisplaySays().c_str());
                apply = g_governor.Target();
            }
            else
                std::snprintf(what, sizeof what, "the sun's shadows %s: %s", g_governor.Shadows() ? "on" : "off",
                              g_governor.Why().c_str());
            LOGI("adaptive: %s (the window: %.1f frames a second, the graphics chip %.1f ms, the renderer's thread"
                 " %.1f ms, the scene at %u%%)", what, window.fps, window.gpuMs, window.rendererMs, scene.percent);
            g_changes.push_back({ at, what });
            g_shadows.store(g_governor.Shadows(), std::memory_order_relaxed);
        }
        if (play && g_playSeconds - g_said >= kSayEvery)
        {
            g_said = g_playSeconds;
            LOGI("performance: %s; held to %u, the scene at %u%%, the sun's shadows %s", Summary().c_str(),
                 g_governor.Target(), scene.percent,
                 !g_shadowsFollow.load(std::memory_order_relaxed) ? "the player's" : g_governor.Shadows() ? "on" : "off");
        }
        if (g_governor.Display() != display)
        {
            g_display.store(g_governor.Display(), std::memory_order_relaxed);
            vote = true;
        }
    }
    if (apply) Apply(apply);
    else if (vote) Vote();
}

const char* adaptive::WhyNoShadows()
{
    if (!On() || g_shadows.load(std::memory_order_relaxed)) return nullptr;
    return "no window of play has shown the room for them yet, or the last one with them came late;"
           " MW2_SHADOWS=1 keeps them";
}

bool adaptive::GiveUpShadows()
{
    if (!On()) return false;
    std::lock_guard lock(g_lock);
    if (!g_governor.GiveUpShadows()) return false;
    g_shadows.store(false, std::memory_order_relaxed);
    LOGI("adaptive: the sun's shadows off before the scene is drawn smaller: the graphics chip is late");
    g_changes.push_back({ std::chrono::duration<double>(Clock::now() - w.first).count(),
                          "the sun's shadows off: the graphics chip is late" });
    return true;
}

void adaptive::PlayerSetLimit(uint32_t limit)
{
    if (!On()) { if (limit) frame_rate::SetLimit(limit); return; }
    uint32_t apply;
    {
        std::lock_guard lock(g_lock);
        StartLocked();
        const bool shadows = g_governor.Shadows();
        // 0: the runtime's again, from the display's fastest.
        g_governor.Start(limit ? std::vector<uint32_t>{ limit } : g_steps, !limit,
                         g_shadowsFollow.load(std::memory_order_relaxed), Fastest());
        g_display.store(g_governor.Display(), std::memory_order_relaxed);
        // The shadows are as they were: another limit is measured afresh,
        // and takes them away itself if they no longer fit.
        if (shadows) g_governor.KeepShadows();
        apply = g_governor.Target();
        LOGI("adaptive: the frame rate %s; the display %s",
             limit ? "is the player's limit and stays" : "follows the device again", DisplaySays().c_str());
    }
    Apply(apply);
}

uint32_t adaptive::DisplayRate()
{
    // Before the first frame, and with MW2_ADAPTIVE off, the limit's own.
    const uint32_t display = g_display.load(std::memory_order_relaxed);
    return display ? display : frame_rate::Limit();
}

void adaptive::PlayerSetShadows(bool follow)
{
    g_shadowsFollow.store(follow, std::memory_order_relaxed);
    if (!On()) return;
    std::lock_guard lock(g_lock);
    StartLocked();
    g_governor.FollowShadows(follow);
    g_shadows.store(g_governor.Shadows(), std::memory_order_relaxed);
}

adaptive::Now adaptive::Current()
{
    Now now;
    now.scenePercent = g_scenePercent.load(std::memory_order_relaxed);
    std::lock_guard lock(g_lock);
    now.target = g_startedLocked ? g_governor.Target() : frame_rate::Limit();
    now.display = g_startedLocked ? g_governor.Display() : frame_rate::Limit();
    now.follows = g_startedLocked && g_governor.RateFollows();
    now.shadows = g_governor.Shadows();
    if (!g_kept.empty())
    {
        now.fps = g_kept.back().fps;
        now.gpuMs = g_kept.back().gpuMs;
        now.rendererMs = g_kept.back().rendererMs;
        Measured last;
        last.fps = now.fps;
        last.gpuMs = now.gpuMs;
        now.late = Governor::WhatWasLate(last, g_kept.back().target);
        now.age = std::chrono::duration<double>(Clock::now() - g_keptAt).count();
        double gpu = 0;
        size_t gpus = 0;
        for (const Kept& kept : g_kept)
            if (kept.gpuMs >= 0) { gpu += kept.gpuMs; gpus++; }
        if (gpus) now.playGpuMs = gpu / double(gpus);
        now.playSeconds = g_playSeconds;
        now.playMissed = g_playSeconds > 0 ? g_missedSeconds / g_playSeconds : 0;
    }
    return now;
}

std::string adaptive::Report()
{
    std::lock_guard lock(g_lock);
    std::string text;
    char line[320];
    text += "graphics: " + vk::pipeline::DeviceSummary() + "\n";
    if (const double hz = frame_rate::DisplayHz(); hz > 0)
    {
        std::snprintf(line, sizeof line, "display: %.2f Hz\n", hz);
        text += line;
    }
    if (!On()) return text + "adaptive: off (MW2_ADAPTIVE)\n";
    std::snprintf(line, sizeof line, "frame rate: held to %u (%s), the scene at %u%% of its width, the sun's shadows %s\n",
                  g_startedLocked ? g_governor.Target() : frame_rate::Limit(),
                  g_startedLocked && g_governor.RateFollows() ? "following the device" : "the player's limit",
                  g_scenePercent.load(std::memory_order_relaxed),
                  !g_shadowsFollow.load(std::memory_order_relaxed) ? "the player's choice"
                  : g_governor.Shadows() ? "on (measured)" : "off (measured)");
    text += line;
    text += "display mode: " + DisplaySays() + "\n";
    text += "play: " + Summary() + "\n";
    // The last minute a line every ten seconds: where it is now.
    const size_t each = size_t(10.0 / kWindowSeconds);
    for (size_t from = g_kept.size() > 6 * each ? g_kept.size() - 6 * each : 0; from < g_kept.size(); from += each)
    {
        const size_t to = std::min(g_kept.size(), from + each);
        double fps = 0, gpu = 0, renderer = 0;
        for (size_t i = from; i < to; i++)
        {
            fps += g_kept[i].fps;
            gpu += g_kept[i].gpuMs;
            renderer += g_kept[i].rendererMs;
        }
        const double count = double(to - from);
        std::snprintf(line, sizeof line, "  at %.0f s: %.1f frames a second of %u, the graphics chip %.1f ms, the"
                      " renderer's thread %.1f ms, the scene at %u%%%s\n", g_kept[from].at, fps / count,
                      g_kept[from].target, gpu / count, renderer / count, g_kept[from].scenePercent,
                      g_kept[from].shadows ? ", shadows" : "");
        text += line;
    }
    for (size_t i = g_changes.size() > 8 ? g_changes.size() - 8 : 0; i < g_changes.size(); i++)
    {
        std::snprintf(line, sizeof line, "  changed at %.0f s: %s\n", g_changes[i].at, g_changes[i].what.c_str());
        text += line;
    }
    return text;
}

void adaptive::Write()
{
    if (!On()) return;
    std::lock_guard lock(g_lock);
    if (g_playSeconds <= 0) return;
    LOGI("performance: %s; %zu changes of the rate or the shadows", Summary().c_str(), g_changes.size());
}

adaptive::Session adaptive::SessionSoFar()
{
    std::lock_guard lock(g_lock);
    return g_tally.Result();
}
