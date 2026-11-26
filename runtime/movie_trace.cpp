// MW2_TRACE_MOVIES=1 (movie_trace.h): the hooks on the title's Bink and the
// mixer's line.
//
// Bink is the title's own code and paces itself: BinkWait compares a
// millisecond timer with the frame's due time, scaled by how fast the sound
// track is being taken, and the title decodes and steps when it says so. So
// the picture's clock is read here where the title steps to the next frame,
// and the frame it steps past is the one the next present shows.
#include "movie_trace.h"

#if MW2_DIAGNOSTICS

#include <ppc_recomp_shared.h>
#include "guest.h"
#include "log.h"
#include "title.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace
{
    // The BINK structure's head, the same in Bink 1 and 2.
    constexpr uint32_t kWidth = 0, kHeight = 4, kFrames = 8, kFrameNum = 12, kFrameRate = 20, kFrameRateDiv = 24;
    // BinkOpen's flags that make its first argument something other than a name.
    constexpr uint32_t kFromHandle = 0x00800000, kFromMemory = 0x04000000;
    // A line for every sixteenth mixer frame: twelve a second.
    constexpr uint64_t kSoundEvery = 16;
    constexpr double kSlowReadMs = 20.0;

    double Milliseconds()
    {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    uint32_t Load(uint32_t address)
    {
        const uint8_t* p = guest::Base() + address;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }

    [[maybe_unused]] void Opened(uint32_t source, uint32_t flags, uint32_t bink)
    {
        char name[128] = "memory";
        if (flags & kFromHandle) std::snprintf(name, sizeof name, "handle %08X", source);
        else if (!(flags & kFromMemory) && source)
        {
            const char* text = reinterpret_cast<const char*>(guest::Base() + source);
            size_t i = 0;
            for (; i + 1 < sizeof name && text[i] >= 0x20 && text[i] < 0x7F; i++) name[i] = text[i];
            name[i] = 0;
        }
        if (!bink) { LOGI("movies: %s did not open (flags %08X), at %.1f ms", name, flags, Milliseconds()); return; }
        const uint32_t divisor = Load(bink + kFrameRateDiv);
        LOGI("movies: opened %s (flags %08X), %ux%u, %u frames at %.3f a second, at %.1f ms",
             name, flags, Load(bink + kWidth), Load(bink + kHeight), Load(bink + kFrames),
             divisor ? double(Load(bink + kFrameRate)) / double(divisor) : 0.0, Milliseconds());
    }

    // The decode of the frame the title will step past next. One movie plays
    // at a time and one thread drives it, so this is not kept per movie.
    [[maybe_unused]] std::atomic<double> g_decodeBegan{ 0.0 }, g_decodeMs{ 0.0 };

    [[maybe_unused]] void Stepped(uint32_t bink)
    {
        if (!bink) return;
        LOGI("movies: frame %u at %.1f ms, decoded in %.2f ms",
             Load(bink + kFrameNum), Milliseconds(), g_decodeMs.load(std::memory_order_relaxed));
    }
}

bool movies::On()
{
    static const bool on = diag::Flag("MW2_TRACE_MOVIES");
    return on;
}

double movies::Clock() { return Milliseconds(); }

void movies::Read(const char* name, uint32_t bytes, long long offset, double tookMs)
{
    // .bik, and Call of Duty 4's .bikbe; the streamed sounds' files, whose
    // speech plays beside an engine-drawn scene; and any other file's read
    // slow enough to hold either up.
    if (!std::strstr(name, ".bik") && !std::strstr(name, "soundfile") && tookMs < kSlowReadMs) return;
    LOGI("movies: read %s, %u bytes at %lld, at %.1f ms, in %.2f ms", name, bytes, offset, Milliseconds(), tookMs);
}

void movies::Sound(double queuedMs, double deviceMs, uint64_t underruns)
{
    if (!On()) return;
    static std::atomic<uint64_t> frames{ 0 };
    const uint64_t frame = frames.fetch_add(1, std::memory_order_relaxed);
    if (frame % kSoundEvery) return;
    LOGI("movies: sound %llu at %.1f ms, %.1f ms queued, %.1f ms in the device, %llu underruns",
         (unsigned long long)frame, Milliseconds(), queuedMs, deviceMs, (unsigned long long)underruns);
}

#ifdef T_Bink_Open
GUEST_HOOK(T_Bink_Open)
{
    const uint32_t source = ctx.r3.u32, flags = ctx.r4.u32;
    GUEST_ORIG(T_Bink_Open)(ctx, base);
    if (movies::On()) Opened(source, flags, ctx.r3.u32);
}

GUEST_HOOK(T_Bink_NextFrame)
{
    if (movies::On()) Stepped(ctx.r3.u32);
    GUEST_ORIG(T_Bink_NextFrame)(ctx, base);
}
#endif

#ifdef T_Bink_DoFrame
GUEST_HOOK(T_Bink_DoFrame)
{
    const double began = Milliseconds();
    GUEST_ORIG(T_Bink_DoFrame)(ctx, base);
    g_decodeMs.store(Milliseconds() - began, std::memory_order_relaxed);
}
#endif

#ifdef T_Bink_DoFrameAsync
// Bink 2 decodes on its own two threads: the title starts a frame and polls
// for its end, a millisecond's sleep between polls.
GUEST_HOOK(T_Bink_DoFrameAsync)
{
    g_decodeBegan.store(Milliseconds(), std::memory_order_relaxed);
    GUEST_ORIG(T_Bink_DoFrameAsync)(ctx, base);
}

GUEST_HOOK(T_Bink_DoFrameAsyncWait)
{
    GUEST_ORIG(T_Bink_DoFrameAsyncWait)(ctx, base);
    if (ctx.r3.u32)
        g_decodeMs.store(Milliseconds() - g_decodeBegan.load(std::memory_order_relaxed), std::memory_order_relaxed);
}
#endif

#endif  // MW2_DIAGNOSTICS
