// The microphone, for voice chat (kernel/voice.cpp): an AAudio input stream in
// callback mode, the mirror of the sink in audio.cpp.
//
// The voice code wants 16 kHz mono 16-bit samples and takes them on a thread
// of its own, so AAudio's callback and that thread meet at a single-producer
// single-consumer ring, as the sink's two threads do. What the ring cannot
// hold is dropped: speech that is a second late is worth less than none.
//
// Nothing here asks for the permission. The app does, when the player turns
// voice chat on (SettingsActivity.kt); without it the stream does not open or
// delivers silence, and the game carries on with nobody hearing this player.
#ifdef MW2_ANDROID

#include "android.h"
#include "../log.h"

#include <aaudio/AAudio.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace
{
    constexpr uint32_t kVoiceRate = 16000;
    constexpr uint32_t kRingSamples = kVoiceRate;      // one second

    int16_t g_ring[kRingSamples];
    std::atomic<uint32_t> g_write{ 0 };
    std::atomic<uint32_t> g_read{ 0 };

    std::mutex g_streamLock;          // opening and closing only
    AAudioStream* g_stream = nullptr;
    std::atomic<bool> g_talking{ false };

    // How many device samples make one of ours: 1, or 3 when the device would
    // only open at 48 kHz. Touched by the callback alone once the stream runs.
    uint32_t g_decimate = 1;
    int32_t g_sum = 0;
    uint32_t g_summed = 0;

    uint32_t Held()
    {
        return g_write.load(std::memory_order_acquire) - g_read.load(std::memory_order_acquire);
    }

    void Push(int16_t sample)
    {
        const uint32_t w = g_write.load(std::memory_order_relaxed);
        if (w - g_read.load(std::memory_order_acquire) >= kRingSamples) return;
        g_ring[w % kRingSamples] = sample;
        g_write.store(w + 1, std::memory_order_release);
    }

    aaudio_data_callback_result_t OnData(AAudioStream*, void* /*user*/, void* audioData,
                                         int32_t numFrames)
    {
        const auto* in = static_cast<const int16_t*>(audioData);
        for (int32_t i = 0; i < numFrames; i++)
        {
            if (g_decimate == 1) { Push(in[i]); continue; }
            // Averaging three samples is a crude low-pass, and enough of one
            // for speech that the title's codec band-limits again anyway.
            g_sum += in[i];
            if (++g_summed < g_decimate) continue;
            Push(int16_t(g_sum / int32_t(g_decimate)));
            g_sum = 0;
            g_summed = 0;
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    void OnError(AAudioStream*, void* /*user*/, aaudio_result_t error)
    {
        // Not reopened: a microphone that went away mid-match (a headset
        // unplugged) leaves the player silent until the next run, which is the
        // safe way round for a microphone to fail.
        LOGW("voice: the microphone stream was disconnected (%s)", AAudio_convertResultToText(error));
    }

    // The voice-communication preset turns the phone's echo canceller and
    // noise suppressor on, without which the speaker's sound goes straight
    // back out through the microphone. The call arrived in Android 9, so it is
    // looked up by name for the reason audio.cpp's DescribeStream gives.
    void DescribeStream(AAudioStreamBuilder* builder)
    {
        using SetInputPreset = void (*)(AAudioStreamBuilder*, aaudio_input_preset_t);
        static SetInputPreset setInputPreset =
            reinterpret_cast<SetInputPreset>(dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setInputPreset"));
        if (setInputPreset) setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    }

    AAudioStream* OpenAt(uint32_t rate)
    {
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) return nullptr;
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(builder, 1);
        AAudioStreamBuilder_setSampleRate(builder, int32_t(rate));
        DescribeStream(builder);
        AAudioStreamBuilder_setDataCallback(builder, OnData, nullptr);
        AAudioStreamBuilder_setErrorCallback(builder, OnError, nullptr);
        AAudioStream* stream = nullptr;
        const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK || !stream)
        {
            LOGW("voice: no microphone at %u Hz (%s)", rate, AAudio_convertResultToText(result));
            return nullptr;
        }
        // A stream asked for one rate and format opens with them or not at
        // all, but a device that answers with something else would be read
        // as noise, so it is checked rather than trusted.
        if (uint32_t(AAudioStream_getSampleRate(stream)) != rate ||
            AAudioStream_getChannelCount(stream) != 1 ||
            AAudioStream_getFormat(stream) != AAUDIO_FORMAT_PCM_I16)
        {
            LOGW("voice: the microphone opened as %d Hz, %d channels, format %d instead of %u Hz mono 16-bit",
                 int(AAudioStream_getSampleRate(stream)), int(AAudioStream_getChannelCount(stream)),
                 int(AAudioStream_getFormat(stream)), rate);
            AAudioStream_close(stream);
            return nullptr;
        }
        return stream;
    }
}

bool android::voice::Open()
{
    std::lock_guard lock(g_streamLock);
    if (g_stream) return true;
    g_write.store(0, std::memory_order_relaxed);
    g_read.store(0, std::memory_order_relaxed);
    g_sum = 0;
    g_summed = 0;

    g_decimate = 1;
    AAudioStream* stream = OpenAt(kVoiceRate);
    if (!stream)
    {
        g_decimate = 3;
        stream = OpenAt(kVoiceRate * 3);
    }
    if (!stream) return false;

    const aaudio_result_t result = AAudioStream_requestStart(stream);
    if (result != AAUDIO_OK)
    {
        LOGW("voice: the microphone would not start (%s)", AAudio_convertResultToText(result));
        AAudioStream_close(stream);
        return false;
    }
    g_stream = stream;
    LOGI("voice: AAudio microphone at %u Hz mono 16-bit%s", kVoiceRate * g_decimate,
         g_decimate == 1 ? "" : ", brought down to 16 kHz");
    return true;
}

void android::voice::Close()
{
    std::lock_guard lock(g_streamLock);
    if (!g_stream) return;
    AAudioStream_requestStop(g_stream);
    AAudioStream_close(g_stream);
    g_stream = nullptr;
}

uint32_t android::voice::Available() { return std::min(Held(), kRingSamples); }

uint32_t android::voice::Read(int16_t* out, uint32_t samples)
{
    const uint32_t take = std::min(Available(), samples);
    const uint32_t r = g_read.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < take; i++) out[i] = g_ring[(r + i) % kRingSamples];
    g_read.store(r + take, std::memory_order_release);
    return take;
}

void android::voice::SetTalking(bool talking) { g_talking.store(talking, std::memory_order_relaxed); }

bool android::voice::Talking() { return g_talking.load(std::memory_order_relaxed); }

#endif  // MW2_ANDROID
