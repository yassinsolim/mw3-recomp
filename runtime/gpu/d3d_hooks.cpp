// MW2's D3D9 is statically linked but not inlined: the engine reaches it
// through ordinary function calls, and a few of them are hooked here.
#include <ppc_recomp_shared.h>
#include "../pacing_trace.h"
#include "../stutters.h"
#include "title.h"
#include "gpu.h"
#include "internal.h"
#include "../diagnostics.h"
#include "../console.h"
#include "../env.h"
#include "../frame_rate.h"
#include "../live_options.h"
#include "../weapon_timing.h"
#include "../guest.h"
#include "../log.h"
#ifdef MW2_ANDROID
#include "../android/android.h"
#endif

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace
{
    // Whether the title's own limiter holds a limit. Call of Duty 4's does
    // not: its Com_Frame (sub_821DD008) keeps three clocks, one for every
    // third frame, and has each wait 1000 / com_maxfps ms for its own turn,
    // so three frames pass in that time wherever in it they fall. A limit of
    // 45 let 136 a second through, which the display then paced: 119 on a
    // 120 Hz panel. The console never asks it for less than its blank's rate.
#if defined(COD4_TITLE)
    constexpr bool kTitleLimits = false;
#else
    constexpr bool kTitleLimits = true;
#endif

    // Where the title's limiter does not hold, the limit is held here: the
    // present returns no sooner than the limit's period after the one
    // before, as the console's does at its blank, and the title's threads
    // behind it run at that. Counted from when the last was due, not from
    // when it came, so a late wake-up is not added to every frame after it.
    void HoldToLimit()
    {
        const uint32_t limit = frame_rate::Limit();
        if (kTitleLimits || !limit || frame_rate::Mode() != frame_rate::kByLimiter) return;
        static std::chrono::steady_clock::time_point due;   // the presenting thread's own
        const auto period = std::chrono::nanoseconds(1'000'000'000 / limit);
        const auto now = std::chrono::steady_clock::now();
        if (now < due)
        {
            std::this_thread::sleep_until(due);
            due += period;
        }
        else due = now + period;
    }

    // MW2_FPS_LIMIT (frame_rate.h) takes the title off the guest's blank
    // with what the console has for it. D3D's present puts the device's
    // D3DRS_PRESENTINTERVAL in the flip request, and its flip handler retires
    // a frame asked for with D3DPRESENT_INTERVAL_IMMEDIATE when the command
    // processor reaches it instead of queueing it for the blank. The title
    // never sets that state, but a state block put back restores it, so it is
    // set at every present. A limit of 30 stays on the blank and asks D3D for
    // its interval of two instead: its flip handler then retires a frame at
    // the second blank after the one before, as it does for a console title
    // that runs at 30. The title's side is two settings of its own:
    // com_maxfps, its limiter, and r_vsync, which has it take a frame's
    // length from the blanks counted rather than from the clock.
    //
    // What com_maxfps is depends on what paces the frames, and that on the
    // display, whose mode a phone may change in the middle of a run, and on
    // the limit, which the app's menu may (frame_rate::SetLimit): the title
    // is told again whenever the pacing or the limit is another one. Back on
    // the console's pacing it is given its own interval and its own two
    // settings again.
    void Pace(uint32_t device)
    {
        const frame_rate::Pacing pacing = frame_rate::Mode();
        const uint32_t limit = frame_rate::Limit();
        constexpr uint32_t kPresentInterval = T_DEV_PresentInterval;   // in D3D9's device structure
        constexpr uint32_t kIntervalTwo = 2, kIntervalImmediate = 0x80000000;
        // Presents come from one guest thread: the device's interval before
        // anything here wrote one, and whether anything has.
        static const uint32_t own = *GuestPtr<be32>(device + kPresentInterval);
        static bool written = false;
        const uint32_t interval = pacing != frame_rate::kByConsole ? kIntervalImmediate
                                  : frame_rate::ConsoleBlanks() == 2 ? kIntervalTwo : own;
        if (interval != own) written = true;
        if (written) *GuestPtr<be32>(device + kPresentInterval) = interval;

        static uint32_t told = ~0u;
        static bool limited = false;   // com_maxfps and r_vsync are not the title's own
        const uint32_t state = uint32_t(pacing) << 16 | limit;
        if (told == state) return;
        told = state;
        // The display's own blanks hold the frames to its rate; a limiter
        // counting whole milliseconds beside them would let 125 through
        // against 120 shown.
        const uint32_t maxFps = pacing == frame_rate::kByDisplay || !kTitleLimits ? 0 : limit;
        char how[160];
        if (pacing == frame_rate::kByConsole)
        {
            if (limited)
            {
                console::RunNow("reset com_maxfps");
                console::RunNow("reset r_vsync");
                limited = false;
            }
            std::snprintf(how, sizeof how, frame_rate::ConsoleBlanks() == 2
                              ? "the console's at half rate: a frame retired at every second guest blank, 30 a second"
                              : "the console's: a frame retired at every guest blank, 60 a second");
        }
        else
        {
            char command[32];
            std::snprintf(command, sizeof command, "com_maxfps %u", maxFps);
            console::RunNow(command);
            console::RunNow("r_vsync 0");
            limited = true;
            if (pacing == frame_rate::kByDisplay)
                std::snprintf(how, sizeof how, "the display's: com_maxfps 0, every frame shown, one a blank");
            else if (limit && kTitleLimits)
                std::snprintf(how, sizeof how, "the title's limiter: com_maxfps %u, %u a second, the newest"
                              " frame shown at each blank", limit, 1000 / std::max(1u, 1000 / limit));
            else if (limit)
                std::snprintf(how, sizeof how, "the runtime's limiter: com_maxfps 0, a present held until %u"
                              " us after the one before, the newest frame shown at each blank",
                              1'000'000 / limit);
            else
                std::snprintf(how, sizeof how, "none: com_maxfps 0, the newest frame shown at each blank");
        }
        if (const double period = frame_rate::DisplayPeriodNs(); period > 0)
            LOGI("pacing: MW2_FPS_LIMIT %u on a display at %.2f Hz, a blank every %.1f us -- %s",
                 limit, frame_rate::DisplayHz(), period / 1000.0, how);
        else
            LOGI("pacing: MW2_FPS_LIMIT %u with no display -- %s", limit, how);
    }

    // MW2_SHADOWS=0: the sun casts no shadows (the title's sm_enable); 1
    // leaves them on. The shadow maps are a fifth of the GPU's frame in a
    // Modern Warfare 3 Survival level on an Adreno 650 -- 38 ms a frame with
    // them, 31 without -- and 6 ms of Modern Warfare 2's S.S.D.D., 45 frames
    // a second with them and 58 without (docs/android.md): the difference
    // between holding the cap that class of chip starts at and not, so unset
    // is off there. Their passes are three in ten of Modern Warfare 3's draws
    // too, which the ring consumer has no time for above 60 a second: on an
    // AYN Thor (Adreno 740) under a limit of 120 Survival ran at 107 with 39%
    // of its frames on time, and at 114.5 with 80% without them, so unset is
    // off for that title under Android wherever the limit is above 60, and
    // for Advanced Warfare, whose build is that title's kind: Seoul's drop
    // pod there ran at 114.4 with 78% on time with them and 118.8 with 90%
    // without. On everywhere else. Said once the title presents, when its console takes
    // commands, and again whenever the answer is another: the app's menu
    // gives the switch another value (live::SetSunShadows) or a limit that
    // decided it.
    constexpr int kUnread = -2, kUnset = -1;
    std::atomic<int> g_shadowsAsked{ kUnread };

    const char* WhyNoShadows()
    {
        int asked = g_shadowsAsked.load(std::memory_order_relaxed);
        if (asked == kUnread)
        {
            const int given = !env::Text("MW2_SHADOWS") ? kUnset : env::Flag("MW2_SHADOWS") ? 1 : 0;
            // A value set while this read the switch is the newer of the two.
            if (g_shadowsAsked.compare_exchange_strong(asked, given, std::memory_order_relaxed)) asked = given;
        }
        if (asked != kUnset) return asked ? nullptr : "MW2_SHADOWS=0";
#if defined(MW2_ANDROID)
        if (android::perf::Adreno6xx())
            return "an Adreno 6xx holds its frame rate without them; MW2_SHADOWS=1 keeps them";
#if defined(MW3_TITLE)
        if (frame_rate::Limit() > frame_rate::kConsole)
            return "a limit above 60 is nearer held without them; MW2_SHADOWS=1 keeps them";
#endif
#endif
        return nullptr;
    }

    void Shadows()
    {
        static std::atomic<bool> told{ true };   // the title starts with them
        const char* why = WhyNoShadows();
        if (told.exchange(!why, std::memory_order_relaxed) == !why) return;
        console::RunNow(why ? "sm_enable 0" : "sm_enable 1");
        if (why) LOGI("shadows: the sun casts none (sm_enable 0): %s", why);
        else LOGI("shadows: the sun casts them again (sm_enable 1)");
    }
}

void live::SetSunShadows(int asked)
{
    g_shadowsAsked.store(asked < 0 ? kUnset : asked ? 1 : 0, std::memory_order_relaxed);
}

// D3D9's present. Its frame is retired the way the console retires it: the
// command stream has the command processor raise an interrupt whose handler
// runs D3D's own flip handler, which counts the frame done.
GUEST_HOOK(T_D3D_Present)
{
    pacing::Note(pacing::kPresent);
    Pace(ctx.r3.u32);
    Shadows();
    weapon_timing::Frame();
    GUEST_ORIG(T_D3D_Present)(ctx, base);
    HoldToLimit();
    pacing::Note(pacing::kPresentEnd);
    gpu::Presented();
    stutters::TitlePresented();
}

namespace
{
    std::atomic<bool>     g_arenaWaitActive{ false };
    std::atomic<uint32_t> g_arenaWaitNeededPos{ 0 };
    std::atomic<uint32_t> g_arenaWaitNeededSeq{ 0 };
    std::atomic<uint64_t> g_arenaWaitStartTimeUs{ 0 };
}

bool gpu::CheckArenaWaitDeadlock(uint64_t maxWaitUs, uint32_t& outSeq, uint32_t& outPos)
{
    if (!g_arenaWaitActive.load(std::memory_order_acquire)) return false;
    const uint64_t start = g_arenaWaitStartTimeUs.load(std::memory_order_relaxed);
    const uint64_t now = pacing::Microseconds();
    if (now - start >= maxWaitUs)
    {
        outSeq = g_arenaWaitNeededSeq.load(std::memory_order_relaxed);
        outPos = g_arenaWaitNeededPos.load(std::memory_order_relaxed);
        return true;
    }
    return false;
}

// D3D9's wait for arena space, which is where the block the command processor
// reports its progress through is learnt (arena.cpp): the device holds a
// pointer to it, and it holds a sequence counter at +0 and a position at +4.
// The device's other fields are in docs/d3d9-seam.md. The wait itself is
// made asleep first (gpu::AwaitArenaProgress); the title's, which spins, then
// finds it over.
GUEST_HOOK(T_D3D_ArenaWait)
{
    constexpr uint32_t kProgressBlock = T_DEV_ProgressBlock;   // in D3D9's device structure
    const uint32_t block = *GuestPtr<be32>(ctx.r3.u32 + kProgressBlock);
    if (block) gpu::SetArenaProgressBlock(block);

    g_arenaWaitNeededPos.store(ctx.r4.u32, std::memory_order_relaxed);
    g_arenaWaitNeededSeq.store(ctx.r5.u32, std::memory_order_relaxed);
    g_arenaWaitStartTimeUs.store(pacing::Microseconds(), std::memory_order_relaxed);
    g_arenaWaitActive.store(true, std::memory_order_release);

    if (block) gpu::AwaitArenaProgress(block, ctx.r4.u32, ctx.r5.u32);
    GUEST_ORIG(T_D3D_ArenaWait)(ctx, base);

    g_arenaWaitActive.store(false, std::memory_order_release);
}

#if MW2_DIAGNOSTICS && defined(T_D3D_ReplayRecording)
namespace
{
    std::atomic<uint64_t> g_endless{ 0 };
}

// A renderer pass' recording is replayed by T_D3D_ReplayRecording, which walks
// the chunk list at [object+116] -- next at +0, an entry count at +4, entries
// from +8 -- and copies each chunk into the command-buffer arena. The inner
// loop counts up to [chunk+4] with a do-while, so a chunk holding no entries is
// 2^32 of them and one whose count is not a count is as good as forever. The
// thread does not come back to be diagnosed, so the list is checked on the way in.
GUEST_HOOK(T_D3D_ReplayRecording)
{
    const uint32_t object = ctx.r4.u32;
    const uint32_t head = object ? uint32_t(*GuestPtr<be32>(object + 116)) : 0;
    uint32_t nodes = 0, entries = 0;
    bool endless = false;
    // Tortoise and hare: a cycle is as endless as a bad count.
    for (uint32_t slow = head, fast = head; fast && !endless; nodes++)
    {
        const uint32_t count = *GuestPtr<be32>(slow + 4);
        if (!count || count > (1u << 20)) { endless = true; break; }
        entries += count;
        fast = *GuestPtr<be32>(fast);
        if (fast) fast = *GuestPtr<be32>(fast);
        slow = *GuestPtr<be32>(slow);
        if (!slow) break;
        if (slow == fast || nodes > 4096) endless = true;
    }
    if (endless && g_endless++ < 8)
        LOGW("d3d9: a recorded command buffer D3D9 cannot walk to the end of"
             " (object %08X, head %08X, %u chunks, %u entries)", object, head, nodes, entries);
    GUEST_ORIG(T_D3D_ReplayRecording)(ctx, base);
}

void gpu::detail::ReportD3DHooks()
{
    if (uint64_t n = g_endless.load())
        LOGI("d3d9: %llu recordings with no end", (unsigned long long)n);
}
#else
void gpu::detail::ReportD3DHooks() {}
#endif
