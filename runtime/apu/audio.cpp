// The audio render driver: the 5.333 ms mixer cadence and the host output.
//
// Pacing is by the wall clock, with the device as the arbiter: when SDL's queue
// runs low the callback is pumped early (the device clock is a touch faster
// than ours, or a burst of output is due), and when it grows past a limit the
// frames are dropped (the device has stalled, or there is none); the callback
// still runs at the nominal rate either way, because the title's sound state
// machines advance inside it and a silent run must still finish its sounds.
#include <ppc_recomp_shared.h>
#include "audio.h"
#include "../diagnostics.h"
#include "../env.h"
#include "../guest.h"
#include "../log.h"
#include "../movie_trace.h"
#include "../crash.h"
#include "../platform.h"
#include "../kernel/kernel.h"
#include "../kernel/voice.h"

#ifdef MW2_USE_SDL
#include <SDL3/SDL.h>
#endif

#ifdef MW2_ANDROID
#include "../android/android.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr uint32_t kFrequency = 48000;
    constexpr uint32_t kChannels = 6;
    constexpr uint32_t kSamplesPerChannel = 256;
    constexpr uint32_t kFrameFloats = kChannels * kSamplesPerChannel;
    constexpr uint32_t kFrameBytes = kFrameFloats * sizeof(float);
    // 256 samples of 48000 a second. To the nanosecond: at 5333 microseconds
    // the mixer made 62 samples in a million more than a device plays.
    constexpr auto     kFrameInterval = std::chrono::nanoseconds(5333333);

    // Queue bounds on the host side, in frames of 5.333 ms.
#ifdef MW2_ANDROID
    constexpr uint32_t kHighWaterFrames = 48;   // drop above this (256 ms)
    constexpr int      kMostCaughtUp = 40;      // frames mixed late before the clock is taken afresh (213 ms)
#else
    constexpr uint32_t kLowWaterFrames  = 3;    // pump early below this
    constexpr uint32_t kHighWaterFrames = 24;   // drop above this (128 ms)
#endif

    constexpr uint32_t kMaxClients = 8;
    constexpr uint32_t kHandleTag = 0x41550000u;
    // Twice what a guest thread gets: the mixer's frames are deep, and an
    // overflow here lands in whichever allocation sits below the stack.
    constexpr uint32_t kStackSize = 1024u * 1024u;

    struct Client
    {
        bool inUse = false;
        uint32_t callback = 0;
        uint32_t context = 0;
        Clock::time_point next{};
    };

    std::mutex g_lock;
    std::condition_variable g_wake;
    Client g_clients[kMaxClients];
    bool g_workerStarted = false;

    diag::Stat g_callbacks, g_frames, g_dropped, g_earlyPumps;   // the audio thread's
    std::atomic<bool> g_outputOpen{ false };
#ifdef MW2_ANDROID
    // The frame being mixed was due more than a frame ago (WorkerMain).
    std::atomic<bool> g_catchingUp{ false };
#endif

#ifdef MW2_USE_SDL
    SDL_AudioStream* g_stream = nullptr;
    std::mutex g_streamLock;
#endif

    // MW2_NO_AUDIO=1: no playback device; the mixer and the decoder still run.
    bool OutputDisabled()
    {
        static const bool off = env::Flag("MW2_NO_AUDIO");
        return off;
    }

    // MW2_AUDIO_DUMP=<file.wav>: the title's mix as it submits it, six
    // channels of 32-bit floats at 48 kHz, to check that a run nobody listens
    // to (a scripted one, a device on a desk, a PC with no sound device) has
    // sound and what kind. Every frame is in it, those the output had no
    // room for too. The header is rewritten every second of sound, because
    // a run ends by the process being killed.
    void Dump(uint32_t samplesGuestAddress)
    {
        static const char* path = diag::Text("MW2_AUDIO_DUMP");
        if (!path) return;
        static std::FILE* file = std::fopen(path, "wb");
        static uint32_t bytes = 0, frames = 0;
        if (!file)
        {
            static bool warned = false;
            if (!warned) { warned = true; LOGW("audio: could not write %s", path); }
            return;
        }
        // The host is little-endian, as the file is.
        struct [[gnu::packed]] Header
        {
            char riff[4]; uint32_t riffSize; char wave[4];
            char fmt[4]; uint32_t fmtSize; uint16_t tag, channels; uint32_t rate, byteRate;
            uint16_t blockAlign, bits; char data[4]; uint32_t dataSize;
        };
        // Six planes of 256 big-endian floats to interleaved host floats.
        const uint32_t* in = reinterpret_cast<const uint32_t*>(guest::Base() + samplesGuestAddress);
        float frame[kFrameFloats];
        for (uint32_t s = 0; s < kSamplesPerChannel; s++)
            for (uint32_t c = 0; c < kChannels; c++)
            {
                uint32_t bits; std::memcpy(&bits, &in[c * kSamplesPerChannel + s], 4);
                bits = __builtin_bswap32(bits);
                std::memcpy(&frame[s * kChannels + c], &bits, 4);
            }
        if (frames == 0) { const Header blank{}; std::fwrite(&blank, sizeof blank, 1, file); }
        std::fwrite(frame, sizeof(float), kFrameFloats, file);
        bytes += kFrameFloats * sizeof(float);
        if (frames++ % (48000 / kSamplesPerChannel) == 0)
        {
            const Header header{ { 'R', 'I', 'F', 'F' }, 36 + bytes, { 'W', 'A', 'V', 'E' },
                                 { 'f', 'm', 't', ' ' }, 16, 3 /* IEEE float */, uint16_t(kChannels), 48000,
                                 48000 * kChannels * 4, uint16_t(kChannels * 4), 32, { 'd', 'a', 't', 'a' }, bytes };
            std::fseek(file, 0, SEEK_SET);
            std::fwrite(&header, sizeof header, 1, file);
            std::fseek(file, 0, SEEK_END);
            std::fflush(file);
        }
    }

    void OpenOutput()
    {
#ifdef MW2_ANDROID
        if (OutputDisabled()) { LOGI("audio: output disabled (MW2_NO_AUDIO)"); return; }
        // AAudio, in its low-latency mode where the device has one. The sink
        // does the 5.1-to-stereo fold and any rate conversion the device
        // needs (runtime/android/audio.cpp).
        if (!android::audio::Open())
        {
            LOGW("audio: no AAudio stream; frames are discarded");
            return;
        }
        g_outputOpen = true;
        LOGI("audio: AAudio open; feeding 5.1 float at 48 kHz");
#elif defined(MW2_USE_SDL)
        if (OutputDisabled()) { LOGI("audio: output disabled (MW2_NO_AUDIO)"); return; }
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            LOGW("audio: SDL audio would not start (%s)", SDL_GetError());
            return;
        }
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32;
        spec.channels = int(kChannels);
        spec.freq = int(kFrequency);
        std::lock_guard<std::mutex> guard(g_streamLock);
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!g_stream)
        {
            LOGW("audio: no playback device (%s); the driver is %s", SDL_GetError(), SDL_GetCurrentAudioDriver());
            return;
        }
        SDL_AudioSpec deviceSpec{};
        int deviceFrames = 0;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_stream), &deviceSpec, &deviceFrames);
        SDL_ResumeAudioStreamDevice(g_stream);
        g_outputOpen = true;
        LOGI("audio: %s, device %d Hz %d ch, %d-sample buffers; feeding 5.1 float at 48 kHz",
             SDL_GetCurrentAudioDriver(), deviceSpec.freq, deviceSpec.channels, deviceFrames);
#else
        LOGI("audio: built without SDL; frames are discarded");
#endif
    }

    // Frames of 5.333 ms the host still holds, or 0 without a device.
    uint32_t QueuedFrames()
    {
#ifdef MW2_ANDROID
        return android::audio::QueuedFrames();
#elif defined(MW2_USE_SDL)
        std::lock_guard<std::mutex> guard(g_streamLock);
        if (!g_stream) return 0;
        int bytes = SDL_GetAudioStreamQueued(g_stream);
        return bytes > 0 ? uint32_t(bytes) / kFrameBytes : 0;
#else
        return 0;
#endif
    }

    // The worker owns a guest stack and thread block: the callback is guest
    // code and reads its thread id out of r13 like any other.
    struct GuestCaller
    {
        uint32_t stackBase = 0, stackTop = 0, pcr = 0;

        bool Prepare()
        {
            stackBase = kernel::AllocateGuest(kStackSize, 0x1000);
            const uint32_t id = kernel::NewThreadId();
            pcr = kernel::CreateThreadBlock(id);
            if (!stackBase || !pcr) return false;
            stackTop = (stackBase + kStackSize - 0x100) & ~0xFu;
            return true;
        }

        void Call(uint32_t address, uint32_t a0)
        {
            PPCFunc* fn = kernel::GuestFunction(address);
            if (!fn)
            {
                static bool warned = false;
                if (!warned) { warned = true; LOGW("audio: no recompiled function at callback %08X", address); }
                return;
            }
            PPCContext ctx{};
            ctx.r1.u32 = stackTop;
            ctx.r13.u32 = pcr;
            ctx.r3.u32 = a0;
            ctx.fpscr.loadFromHost();
            fn(ctx, guest::Base());
        }
    };

    void WorkerMain()
    {
        crash::RegisterThread("audio worker");
        GuestCaller caller;
        if (!caller.Prepare()) { LOGE("audio: could not set up the worker's guest state"); return; }
        OpenOutput();

#ifndef MW2_ANDROID
        Clock::time_point lastPump = Clock::now();
#endif
        for (;;)
        {
            uint32_t callback = 0, context = 0;
            {
                std::unique_lock<std::mutex> guard(g_lock);
                Client* pick = nullptr;
                for (auto& c : g_clients)
                    if (c.inUse && (!pick || c.next < pick->next)) pick = &c;
                if (!pick)
                {
                    g_wake.wait(guard);
                    continue;
                }
                const auto now = Clock::now();
#ifdef MW2_ANDROID
                // Android's clock: no early pump.
                if (pick->next > now)
                {
                    g_wake.wait_until(guard, pick->next);
                    continue;
                }
                // A frame that comes late is still mixed, and the ones due
                // since are mixed straight after it: the title's sound runs
                // on the count of these frames, and one that was passed over
                // left every sound playing that much behind the picture for
                // the rest of its length (a cutscene's whole sound track is
                // one sound). Behind by a lot, the process was stopped.
                if (pick->next + kMostCaughtUp * kFrameInterval < now) pick->next = now;
                g_catchingUp.store(pick->next + kFrameInterval < now, std::memory_order_relaxed);
                // The device plays on its own clock, which is not this one
                // to a part in ten thousand, and what the mixer makes beyond
                // what the device plays stays in the sink for good: 3.3 ms
                // more every minute on the Retroid Pocket Flip 2 (the 5333
                // microseconds the interval was, and the clocks), the sound
                // 100 ms behind the picture after half an hour, which a
                // cutscene's speech shows. So the mixer follows the device:
                // half a hundredth slower while the sink holds more than it
                // does when both are on time, as much faster while it is
                // empty when a frame is due.
                auto interval = kFrameInterval;
                if (g_outputOpen)
                {
                    const uint32_t held = android::audio::QueuedFrames();
                    if (held > android::audio::CushionFrames()) interval += kFrameInterval / 200;
                    else if (held == 0) interval -= kFrameInterval / 200;
                }
                pick->next += interval;
#else
                Clock::time_point due = pick->next;
                // Behind by a lot (the process was stopped): resynchronise rather
                // than pump a burst.
                if (due + 40 * kFrameInterval < now) due = now;
                const bool starving = QueuedFrames() < kLowWaterFrames && g_outputOpen &&
                                      now - lastPump >= std::chrono::milliseconds(1);
                if (due > now && !starving)
                {
                    g_wake.wait_until(guard, due);
                    continue;
                }
                if (starving && due > now) g_earlyPumps++;
                pick->next = std::max(due, now - kFrameInterval) + kFrameInterval;
#endif
                callback = pick->callback;
                context = pick->context;
            }
#ifndef MW2_ANDROID
            lastPump = Clock::now();
#endif
            g_callbacks++;
            // The callback takes the context it registered: the title's own object.
            caller.Call(callback, context);
        }
    }

    void EnsureWorker()
    {
        if (g_workerStarted) return;
        g_workerStarted = true;
        // The mixer callback is guest code and runs on this thread, so the
        // thread needs a host stack the guest's frames fit in (platform.h).
        if (!platform::StartThread([](void*) { WorkerMain(); }, nullptr,
                                   8u * 1024u * 1024u, "audio mixer"))
        {
            LOGE("audio: the mixer thread could not be started");
            g_workerStarted = false;
        }
    }
}

uint32_t apu::audio::RegisterClient(uint32_t callback, uint32_t callbackContext)
{
    std::lock_guard<std::mutex> guard(g_lock);
    for (uint32_t i = 0; i < kMaxClients; i++)
    {
        Client& c = g_clients[i];
        if (c.inUse) continue;
        c.callback = callback;
        c.context = callbackContext;
#if defined(AW_TITLE) || defined(BO3_TITLE)
        // This XAudio2 (XDK 21256) registers a callback that starts by loading
        // its object from where its argument points (`lwz r3,0(r3)`), and the
        // context it registers is not such a place: what stands there is the
        // object's table of functions. So the driver hands the callback the
        // address of a word that holds the context, as Xenia's does for every
        // title. Handed the context itself, the mixer waited for ever on an
        // "event" read out of that table, and no frame was ever submitted.
        // The earlier titles' callbacks are served the context as it is.
        if (const uint32_t cell = kernel::AllocateGuest(4, 4))
        {
            *GuestPtr<be32>(cell) = callbackContext;
            c.context = cell;
        }
#endif
        // The first callback only once the registering call has returned and
        // the title has finished setting its object up.
        c.next = Clock::now() + std::chrono::milliseconds(100);
        c.inUse = true;
        EnsureWorker();
        g_wake.notify_all();
        LOGK("audio: client %u registered, callback %08X context %08X", i, callback, callbackContext);
        return kHandleTag | i;
    }
    LOGW("audio: no free client slot");
    return 0;
}

void apu::audio::UnregisterClient(uint32_t handle)
{
    const uint32_t i = handle & 0xFFFF;
    if ((handle & 0xFFFF0000u) != kHandleTag || i >= kMaxClients) return;
    std::lock_guard<std::mutex> guard(g_lock);
    Client& c = g_clients[i];
    if (!c.inUse) return;
    c = Client{};
    LOGK("audio: client %u unregistered", i);
}

void apu::audio::SubmitFrame(uint32_t handle, uint32_t samplesGuestAddress)
{
    const uint32_t i = handle & 0xFFFF;
    if ((handle & 0xFFFF0000u) != kHandleTag || i >= kMaxClients || !samplesGuestAddress) return;
    g_frames++;
    Dump(samplesGuestAddress);
    if (movies::On())
    {
        double queuedMs = QueuedFrames() * (kSamplesPerChannel * 1000.0 / kFrequency), deviceMs = -1.0;
        uint64_t underruns = 0;
#ifdef MW2_ANDROID
        android::audio::Latency(queuedMs, deviceMs);
        underruns = android::audio::Underruns();
#endif
        movies::Sound(queuedMs, deviceMs, underruns);
    }
#ifdef MW2_ANDROID
    if (!g_outputOpen) return;
    if (android::audio::QueuedFrames() > kHighWaterFrames) { g_dropped++; return; }
    // The device played silence where a late frame should have been, so the
    // frames mixed late are owed to nobody once the sink holds what it does
    // when the mixer is on time: played as well, they would stay in the sink
    // and every sound after them would be that much later for good.
    if (g_catchingUp.load(std::memory_order_relaxed) &&
        android::audio::QueuedFrames() >= android::audio::CushionFrames()) { g_dropped++; return; }
    {
        // Six planes of 256 big-endian floats to interleaved host floats,
        // the same conversion the SDL path makes; the sink folds them down.
        const uint32_t* in = reinterpret_cast<const uint32_t*>(guest::Base() + samplesGuestAddress);
        alignas(16) float out[kFrameFloats];
        for (uint32_t s = 0; s < kSamplesPerChannel; s++)
            for (uint32_t c = 0; c < kChannels; c++)
            {
                uint32_t bits; std::memcpy(&bits, &in[c * kSamplesPerChannel + s], 4);
                bits = __builtin_bswap32(bits);
                std::memcpy(&out[s * kChannels + c], &bits, 4);
            }
        // The other players' voices are no part of the title's mix: the
        // console sent them to the headset (kernel/voice.cpp). Into the
        // first client's frames only, or two clients would play them twice.
        if (i == 0) kernel::voice::Mix(out, kSamplesPerChannel, kChannels);
        android::audio::Write(out, kSamplesPerChannel);
    }
    return;
#endif
#ifdef MW2_USE_SDL
    std::lock_guard<std::mutex> guard(g_streamLock);
    if (!g_stream) return;
    const int queued = SDL_GetAudioStreamQueued(g_stream);
    if (queued > int(kHighWaterFrames * kFrameBytes)) { g_dropped++; return; }

    // Six planes of 256 big-endian floats to interleaved host floats.
    const uint32_t* in = reinterpret_cast<const uint32_t*>(guest::Base() + samplesGuestAddress);
    alignas(16) float out[kFrameFloats];
    for (uint32_t s = 0; s < kSamplesPerChannel; s++)
        for (uint32_t c = 0; c < kChannels; c++)
        {
            uint32_t bits; std::memcpy(&bits, &in[c * kSamplesPerChannel + s], 4);
            bits = __builtin_bswap32(bits);
            std::memcpy(&out[s * kChannels + c], &bits, 4);
        }
    // The other players' voices are no part of the title's mix: the console
    // sent them to the headset (kernel/voice.cpp). Into the first client's
    // frames only, or two clients would play them twice.
    if (i == 0) kernel::voice::Mix(out, kSamplesPerChannel, kChannels);
    if (!SDL_PutAudioStreamData(g_stream, out, int(kFrameBytes)))
    {
        static bool warned = false;
        if (!warned) { warned = true; LOGW("audio: put failed (%s)", SDL_GetError()); }
    }
#endif
}

void apu::audio::Report()
{
    if (!g_callbacks) return;
    LOGI("audio: %llu callbacks, %llu frames submitted, %llu dropped, %llu early pumps; output %s",
         (unsigned long long)g_callbacks.load(), (unsigned long long)g_frames.load(),
         (unsigned long long)g_dropped.load(), (unsigned long long)g_earlyPumps.load(),
         g_outputOpen ? "open" : "closed");
}
