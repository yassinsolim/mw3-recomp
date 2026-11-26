// The audio sink, where the desktop build has SDL: an AAudio stream in
// callback mode.
//
// The mixer in apu/audio.cpp runs the title's 5.333 ms cadence and hands over
// 256 samples of six channels at 48 kHz. That producer and AAudio's callback
// are two threads that must never block each other, so between them sits a
// single-producer single-consumer ring of whole mixer frames: the mixer writes
// one and moves on, the callback takes what it needs and outputs silence if
// there is nothing (an underrun is a click; a lock is a dropout).
//
// The device gets stereo: no phone has six speakers, and AAudio would resample
// and downmix in its own mixer thread anyway. The downmix is the usual ITU
// one, with the centre and surrounds at -3 dB.
#ifdef MW2_ANDROID

#include "android.h"
#include "../log.h"

#include <aaudio/AAudio.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

namespace
{
    constexpr uint32_t kGuestRate = 48000;
    constexpr uint32_t kGuestChannels = 6;
    constexpr uint32_t kFrameSamples = 256;            // per channel, one mixer frame
    constexpr uint32_t kRingFrames = 48;               // 256 ms at most
    constexpr uint32_t kOutChannels = 2;

    // The ring holds stereo samples already downmixed, so the callback does
    // nothing but copy (and resample, if the device insisted on another rate).
    struct Ring
    {
        std::vector<float> samples;                    // kRingFrames * kFrameSamples * 2
        std::atomic<uint32_t> write{ 0 };              // in samples-per-channel
        std::atomic<uint32_t> read{ 0 };
        uint32_t capacity = 0;                         // samples per channel

        void Reset(uint32_t frames)
        {
            capacity = frames * kFrameSamples;
            samples.assign(size_t(capacity) * kOutChannels, 0.0f);
            write.store(0, std::memory_order_relaxed);
            read.store(0, std::memory_order_relaxed);
        }

        uint32_t Held() const
        {
            const uint32_t w = write.load(std::memory_order_acquire);
            const uint32_t r = read.load(std::memory_order_acquire);
            return w - r;                              // unsigned wrap is the difference
        }

        uint32_t Free() const { return capacity - std::min(Held(), capacity); }

        void Push(const float* stereo, uint32_t count)
        {
            if (!capacity) return;
            const uint32_t w = write.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < count; i++)
            {
                const uint32_t at = ((w + i) % capacity) * kOutChannels;
                samples[at] = stereo[i * 2];
                samples[at + 1] = stereo[i * 2 + 1];
            }
            write.store(w + count, std::memory_order_release);
        }

        // Takes up to `count`, returns how many it had.
        uint32_t Pop(float* out, uint32_t count)
        {
            if (!capacity) return 0;
            const uint32_t held = std::min(Held(), capacity);
            const uint32_t take = std::min(held, count);
            const uint32_t r = read.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < take; i++)
            {
                const uint32_t at = ((r + i) % capacity) * kOutChannels;
                out[i * 2] = samples[at];
                out[i * 2 + 1] = samples[at + 1];
            }
            read.store(r + take, std::memory_order_release);
            return take;
        }
    };

    std::mutex g_streamLock;          // opening and closing only
    AAudioStream* g_stream = nullptr;
    Ring g_ring;
    std::atomic<bool> g_open{ false };
    std::atomic<bool> g_muted{ false };
    std::atomic<bool> g_needRestart{ false };
    std::atomic<uint64_t> g_underruns{ 0 };
    uint32_t g_deviceRate = kGuestRate;
    // What the sink holds when the mixer is on time: what the device takes
    // at once and a frame more.
    std::atomic<uint32_t> g_deviceFrames{ 2 };

    // How late the mixer has been finishing its frames, in its frames. A
    // processor with more to do than it has cores for runs the mixer's
    // thread, and the title's threads it waits for, tens of milliseconds
    // late several times a second. It then mixes the frames it owes in a
    // burst, of which the sink took what it holds when the mixer is on time
    // and the rest were dropped, and the device played silence until the
    // next burst: on an AYN Thor with every core kept busy by other
    // programs, 152 underruns in 82 seconds of Advanced Warfare's play at 25
    // to 50 frames a second. So the sink holds that much more while it goes on:
    // the third most lateness of this quarter-minute's spells or of the one
    // before's, up to 160 ms. The third, because one or two long ones are a
    // level starting, where the sound would be late by them for half a
    // minute to cover a pause that is over (the AYN Thor's start of Seoul:
    // 27 ms more for the level's first half minute, by the most).
    constexpr uint32_t kMostLateFrames = 30;           // 160 ms
    constexpr int64_t kLatePeriodNs = 15'000'000'000;
    constexpr int64_t kSpellApartNs = 250'000'000;
    constexpr int64_t kFrameNs = 1'000'000'000ll * kFrameSamples / kGuestRate;
    // A period's three longest spells, the longest first, in frames.
    struct Late
    {
        uint32_t worst[3]{};
        void Add(uint32_t frames)
        {
            for (uint32_t& place : worst)
                if (frames > place) std::swap(frames, place);
        }
    };
    Late g_lateNow, g_lateBefore;                      // the mixer's thread
    uint32_t g_spellFrames = 0;                        // the spell under way
    int64_t g_latePeriodEnds = 0, g_lastLateAt = 0, g_spellBegan = 0;   // the mixer's thread
    std::atomic<uint32_t> g_lateFrames{ 0 };

    // Where the resampler is between two ring samples, when the device does
    // not run at 48 kHz. Touched only by the callback.
    double g_resamplePosition = 0.0;
    float g_lastSample[kOutChannels] = { 0, 0 };

    aaudio_data_callback_result_t OnData(AAudioStream*, void* /*user*/, void* audioData,
                                         int32_t numFrames)
    {
        auto* out = static_cast<float*>(audioData);
        const uint32_t wanted = uint32_t(std::max(numFrames, 0));
        if (!wanted) return AAUDIO_CALLBACK_RESULT_CONTINUE;

        if (g_muted.load(std::memory_order_relaxed))
        {
            std::memset(out, 0, size_t(wanted) * kOutChannels * sizeof(float));
            // The ring is still drained, so unpausing does not play a stale
            // second of audio from before the player left the game.
            float discard[64 * kOutChannels];
            uint32_t remaining = wanted;
            while (remaining > 0)
            {
                const uint32_t chunk = std::min(remaining, 64u);
                if (g_ring.Pop(discard, chunk) == 0) break;
                remaining -= chunk;
            }
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }

        if (g_deviceRate == kGuestRate)
        {
            const uint32_t got = g_ring.Pop(out, wanted);
            if (got < wanted)
            {
                std::memset(out + size_t(got) * kOutChannels, 0,
                            size_t(wanted - got) * kOutChannels * sizeof(float));
                g_underruns.fetch_add(1, std::memory_order_relaxed);
            }
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }

        // True linear interpolation resampling, for a device AAudio opened at another rate.
        const double step = double(kGuestRate) / double(g_deviceRate);
        static float nextSample[2] = { 0.0f, 0.0f };
        for (uint32_t i = 0; i < wanted; i++)
        {
            while (g_resamplePosition >= 1.0)
            {
                g_lastSample[0] = nextSample[0];
                g_lastSample[1] = nextSample[1];
                float pair[2];
                if (g_ring.Pop(pair, 1) == 1)
                {
                    nextSample[0] = pair[0];
                    nextSample[1] = pair[1];
                }
                else
                {
                    g_underruns.fetch_add(1, std::memory_order_relaxed);
                    nextSample[0] = nextSample[1] = 0.0f;
                }
                g_resamplePosition -= 1.0;
            }
            const float frac = float(g_resamplePosition);
            out[i * 2]     = g_lastSample[0] + frac * (nextSample[0] - g_lastSample[0]);
            out[i * 2 + 1] = g_lastSample[1] + frac * (nextSample[1] - g_lastSample[1]);
            g_resamplePosition += step;
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    void OnError(AAudioStream*, void* /*user*/, aaudio_result_t error)
    {
        // Headphones unplugged, or the device went away. Closing a stream from
        // its own error callback is not allowed: the mixer thread does it when
        // it next writes.
        LOGW("audio: the stream was disconnected (%s)", AAudio_convertResultToText(error));
        g_needRestart.store(true, std::memory_order_release);
    }

    // What the stream is for. The system routes and ducks by it: a game keeps
    // playing under a notification where music would be quietened.
    //
    // Both calls arrived in Android 9 and the NDK's headers say so, which on
    // a build whose minimum is Android 8 makes the declaration *unavailable*
    // rather than merely new -- and an unavailable declaration cannot be
    // reached at all, not even inside __builtin_available, because the error
    // is about naming it. So they are not named: the symbols are looked up
    // by name in what is already loaded, and skipped on the phones that do
    // not have them. There the stream is treated as media, which changes how
    // the system ducks it and nothing a player would notice.
    void DescribeStream(AAudioStreamBuilder* builder)
    {
        using SetUsage = void (*)(AAudioStreamBuilder*, aaudio_usage_t);
        using SetContentType = void (*)(AAudioStreamBuilder*, aaudio_content_type_t);

        // libaaudio is a dependency of this library, so it is in the global
        // namespace already; this is a lookup, not a load.
        static SetUsage setUsage =
            reinterpret_cast<SetUsage>(dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setUsage"));
        static SetContentType setContentType =
            reinterpret_cast<SetContentType>(dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setContentType"));

        if (setUsage) setUsage(builder, AAUDIO_USAGE_GAME);
        if (setContentType) setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    }

    bool OpenLocked()
    {
        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t result = AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK || !builder)
        {
            LOGW("audio: no AAudio stream builder (%s)", AAudio_convertResultToText(result));
            return false;
        }

        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, int32_t(kOutChannels));
        AAudioStreamBuilder_setSampleRate(builder, int32_t(kGuestRate));
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        DescribeStream(builder);
        AAudioStreamBuilder_setDataCallback(builder, OnData, nullptr);
        AAudioStreamBuilder_setErrorCallback(builder, OnError, nullptr);

        AAudioStream* stream = nullptr;
        result = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK || !stream)
        {
            LOGW("audio: AAudio would not open (%s)", AAudio_convertResultToText(result));
            return false;
        }

        g_deviceRate = uint32_t(std::max(AAudioStream_getSampleRate(stream), 8000));
        const int32_t burst = std::max(AAudioStream_getFramesPerBurst(stream), 32);
        // Three bursts of slack: enough that a late mixer frame is not a click,
        // short enough that a gunshot is not behind its muzzle flash.
        AAudioStream_setBufferSizeInFrames(stream, burst * 3);
        // What the device takes at a time, in the mixer's frames and rounded
        // up, and one more for the mixer's own lateness.
        const uint32_t taken = uint32_t(uint64_t(burst) * kGuestRate / g_deviceRate);
        g_deviceFrames.store((taken + kFrameSamples - 1) / kFrameSamples + 1, std::memory_order_relaxed);
        g_resamplePosition = 1.0;
        g_lastSample[0] = g_lastSample[1] = 0;

        result = AAudioStream_requestStart(stream);
        if (result != AAUDIO_OK)
        {
            LOGW("audio: AAudio would not start (%s)", AAudio_convertResultToText(result));
            AAudioStream_close(stream);
            return false;
        }

        g_stream = stream;
        const bool low = AAudioStream_getPerformanceMode(stream) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
        LOGI("audio: AAudio %d Hz, %d channels, burst %d, a buffer of %d of %d, %s latency; "
             "5.1 is downmixed to stereo",
             int(g_deviceRate), int(AAudioStream_getChannelCount(stream)), int(burst),
             int(AAudioStream_getBufferSizeInFrames(stream)),
             int(AAudioStream_getBufferCapacityInFrames(stream)), low ? "low" : "normal");
        // The system keeps a stream off its fast output when an effect that
        // works on everything played is on (an equaliser application), and
        // nothing asked for here changes that; an output with no fast path
        // (Bluetooth) does the same. The sound is then a tenth of a second
        // and more behind the picture, which a cutscene's speech shows: a
        // movie's sound came 124 ms after its picture on the AYN Thor with
        // JamesDSP on, and 36 ms after with it off.
        if (!low)
            LOGW("audio: the device gave its ordinary output, not its low-latency one: the sound will be "
                 "behind the picture. A system-wide equaliser or sound effect does this, and so do "
                 "Bluetooth headphones");
        return true;
    }

    void CloseLocked()
    {
        if (!g_stream) return;
        AAudioStream_requestStop(g_stream);
        AAudioStream_close(g_stream);
        g_stream = nullptr;
    }
}

bool android::audio::Open()
{
    std::lock_guard lock(g_streamLock);
    if (g_stream) return true;
    g_ring.Reset(kRingFrames);
    const bool ok = OpenLocked();
    g_open.store(ok, std::memory_order_release);
    return ok;
}

void android::audio::Close()
{
    std::lock_guard lock(g_streamLock);
    g_open.store(false, std::memory_order_release);
    CloseLocked();
    LOGI("audio: stopped after %llu underruns", (unsigned long long)g_underruns.load());
}

bool android::audio::IsOpen() { return g_open.load(std::memory_order_acquire); }

void android::audio::SetMuted(bool muted) { g_muted.store(muted, std::memory_order_relaxed); }

uint32_t android::audio::QueuedFrames()
{
    if (!IsOpen()) return 0;
    return g_ring.Held() / kFrameSamples;
}

uint32_t android::audio::DeviceFrames() { return g_deviceFrames.load(std::memory_order_relaxed); }

uint32_t android::audio::CushionFrames()
{
    return g_deviceFrames.load(std::memory_order_relaxed) + g_lateFrames.load(std::memory_order_relaxed);
}

void android::audio::MixedLate(int64_t nanoseconds)
{
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    const int64_t now = int64_t(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
    // The frames mixed straight after a late one are late with it: a spell
    // is over when none has been late for a quarter of a second, and after
    // a second in any case -- a processor that is always busy makes one late
    // frame run into the next, and that is many spells, not one.
    if (g_spellFrames && (now - g_lastLateAt >= kSpellApartNs || now - g_spellBegan >= 4 * kSpellApartNs))
    {
        g_lateNow.Add(g_spellFrames);
        g_spellFrames = 0;
    }
    if (now >= g_latePeriodEnds)
    {
        g_lateBefore = g_lateNow;
        g_lateNow = Late{};
        g_latePeriodEnds = now + kLatePeriodNs;
    }
    // The cushion has a frame for the mixer's lateness already.
    if (nanoseconds > kFrameNs)
    {
        if (!g_spellFrames) g_spellBegan = now;
        g_spellFrames = std::max(g_spellFrames, std::min(kMostLateFrames, uint32_t((nanoseconds - 1) / kFrameNs)));
        g_lastLateAt = now;
    }
    Late withThis = g_lateNow;
    withThis.Add(g_spellFrames);
    const uint32_t wanted = std::max(withThis.worst[2], g_lateBefore.worst[2]);
    const uint32_t had = g_lateFrames.exchange(wanted, std::memory_order_relaxed);
    if (wanted != had && (wanted == 0 || had == 0))
        LOGI("audio: the mixer %s; the sink holds %u ms when it is on time", wanted
             ? "is finishing its frames late (the processor is busy)" : "is on time again",
             unsigned((g_deviceFrames.load(std::memory_order_relaxed) + wanted) * kFrameNs / 1'000'000));
}

bool android::audio::Latency(double& ringMs, double& deviceMs)
{
    // Asked on the mixer's thread, which must not wait for an opening or a
    // closing stream.
    std::unique_lock lock(g_streamLock, std::try_to_lock);
    if (!lock.owns_lock() || !g_stream) return false;
    int64_t position = 0, shownAt = 0;
    if (AAudioStream_getTimestamp(g_stream, CLOCK_MONOTONIC, &position, &shownAt) != AAUDIO_OK) return false;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t nowNs = int64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
    // The frame at `position` reached the speaker at `shownAt`; the one the
    // callback writes next follows it by what lies between them.
    const int64_t ahead = AAudioStream_getFramesWritten(g_stream) - position;
    deviceMs = double(ahead) * 1000.0 / double(g_deviceRate) - double(nowNs - shownAt) / 1e6;
    ringMs = double(g_ring.Held()) * 1000.0 / double(kGuestRate);
    return true;
}

uint64_t android::audio::Underruns() { return g_underruns.load(std::memory_order_relaxed); }

void android::audio::Write(const float* samples, uint32_t frames)
{
    if (!samples || !frames) return;

    // A stream that was disconnected is reopened here, on the mixer's thread.
    if (g_needRestart.exchange(false, std::memory_order_acq_rel))
    {
        std::lock_guard lock(g_streamLock);
        CloseLocked();
        g_ring.Reset(kRingFrames);
        const bool ok = OpenLocked();
        g_open.store(ok, std::memory_order_release);
        if (!ok)
        {
            LOGW("audio: could not reopen the stream; the run continues without sound");
            return;
        }
    }
    if (!IsOpen()) return;

    // Anything the ring cannot take is dropped rather than waited for: the
    // mixer must keep the title's cadence, and the sink being full means the
    // device has stalled.
    const uint32_t room = g_ring.Free();
    if (room < frames) return;

    // 5.1 in the console's order (L, R, C, LFE, Ls, Rs) down to stereo.
    constexpr float kCentre = 0.7071f, kSurround = 0.7071f, kLfe = 0.5f;
    static thread_local std::vector<float> stereo;
    stereo.resize(size_t(frames) * kOutChannels);
    for (uint32_t i = 0; i < frames; i++)
    {
        const float* in = samples + size_t(i) * kGuestChannels;
        float left  = in[0] + kCentre * in[2] + kLfe * in[3] + kSurround * in[4];
        float right = in[1] + kCentre * in[2] + kLfe * in[3] + kSurround * in[5];
        // The downmix can sum past full scale; clipping there is quieter than
        // letting it wrap in the device's converter.
        stereo[i * 2]     = std::clamp(left, -1.0f, 1.0f);
        stereo[i * 2 + 1] = std::clamp(right, -1.0f, 1.0f);
    }
    g_ring.Push(stereo.data(), frames);
}

#endif  // MW2_ANDROID
