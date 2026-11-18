// XamVoice* -- the headset, for voice chat in multiplayer.
//
// The title's voice library (XHV) is linked into the executable and runs as
// recompiled code: the voice activity detector, the speech codec and the
// network framing are all its own, and what it sends rides at the end of the
// title's ordinary datagrams, so the lan and relay services carry it as they
// carry everything else. All it asks of XAM is a headset: somewhere to get
// microphone samples from and somewhere to send the other players' to.
//
// The headset is a queue of packets in each direction (XamVoiceSubmitPacket).
// A packet is 28 bytes of guest memory, described by Packet below, and XHV
// polls its status word rather than waiting on anything.
//
//   Capture (direction 1). XHV hands over a buffer and asks for `size` bytes
//   in the headset's wire format (format 1). Its buffer is four times that
//   size, because it decodes in place to 16 kHz 16-bit samples, and a
//   completed packet whose format reads 0 is taken as already decoded
//   (sub_8251CBA8 at 8251CC84..8251CD64). So the microphone's samples go in
//   as they are, big-endian: four times the bytes asked for, format 0. XHV
//   turns the size back down when it queues the packet again (sub_8251C998).
//
//   Playback (direction 0). XHV has already encoded the samples to the
//   headset's format, 4-bit ADPCM at 16 kHz, with its own coder. That coder
//   is called back to decode them (title.h, T_XHV_AdpcmCoder), which is exact
//   where a second implementation of it here could only be close.
//
// The console's title is open microphone: it has no push-to-talk and sends
// whenever its detector hears speech. So the gate is here. Voice is off unless
// MW2_VOICE=1, and then the microphone's samples are replaced by silence
// unless the push-to-talk key or button is held (MW2_VOICE_PTT=0 for an open
// microphone). Silence is still delivered at the microphone's pace, because
// XHV times its work by the packets coming back.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "kernel.h"
#include "voice.h"
#include "../crash.h"
#include "../diagnostics.h"
#include "../env.h"
#include "../guest.h"
#include "../log.h"

#ifdef MW2_ANDROID
#include "../android/android.h"
#elif defined(MW2_USE_SDL)
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// All of it exists only for a title whose coder has been located (title.h):
// without the coder what the other players say cannot be decoded.
#ifdef T_XHV_AdpcmCoder

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr uint32_t kVoiceRate = 16000;            // XHV's own rate
    constexpr uint32_t kHandle = 0x564F4943u;         // any non-zero value: there is one headset
    constexpr uint32_t kCapture = 1, kPlayback = 0;   // XamVoiceSubmitPacket's direction
    constexpr uint32_t kFormatPcm = 0, kFormatAdpcm = 1;
    // Microphone samples older than this are dropped: XHV takes a packet
    // every 20 ms, and a backlog would be heard as delay for the whole match.
    constexpr uint32_t kMaxBacklog = kVoiceRate / 5;
    // What is held before the other players are heard, so that packets
    // arriving unevenly do not stutter, and the most that is ever held.
    constexpr uint32_t kPlaybackLead = kVoiceRate * 60 / 1000;
    constexpr uint32_t kPlaybackRoom = kVoiceRate / 2;

    struct Packet
    {
        be32 status;       // X_STATUS_PENDING while the headset has it
        be32 done;         // bytes completed
        be32 buffer;
        be32 size;         // bytes
        be32 reserved;
        be32 format;
        be32 references;   // XHV's own
    };

    // MW2_VOICE=1. Off by default: speech leaves the machine only when the
    // player has asked for that.
    bool Enabled()
    {
        static const bool on = env::Flag("MW2_VOICE");
        return on;
    }

    // On unless it is set to 0: the title would otherwise send whatever the
    // room sounds like.
    bool PushToTalk()
    {
        static const bool on = !env::Text("MW2_VOICE_PTT") || env::Flag("MW2_VOICE_PTT");
        return on;
    }

    // MW2_VOICE_TONE=1: a 440 Hz tone stands in for the microphone, so two
    // copies on one machine can be tested without anyone speaking.
    bool Tone()
    {
        static const bool on = diag::Flag("MW2_VOICE_TONE");
        return on;
    }

    // Whether this player means to be heard right now.
    bool Talking()
    {
        if (!PushToTalk()) return true;
#ifdef MW2_ANDROID
        // Nothing is sent from a game the player has left: the button cannot
        // be held there, and a stuck one must not leave the microphone live.
        return android::voice::Talking() && !android::Paused();
#elif defined(MW2_USE_SDL)
        // T: no pad button is bound to it (input.cpp's kKeys). Read here
        // rather than there because it is not a controller's button, and only
        // while the game's window has the keyboard, as the other keys are.
        if (!SDL_WasInit(SDL_INIT_VIDEO) || !SDL_GetKeyboardFocus()) return false;
#ifdef _WIN32
        return (GetAsyncKeyState('T') & 0x8000) != 0;
#else
        const bool* keys = SDL_GetKeyboardState(nullptr);
        return keys && keys[SDL_SCANCODE_T];
#endif
#else
        return false;
#endif
    }

    // ---- the microphone ----------------------------------------------------
    // 16 kHz mono 16-bit, whatever the device runs at. All of it is called
    // with g_lock held.
#if defined(MW2_USE_SDL) && !defined(MW2_ANDROID)
    SDL_AudioStream* g_microphone = nullptr;
#endif
    bool g_microphoneOpen = false;

    void OpenMicrophone()
    {
        if (g_microphoneOpen || Tone()) return;
#ifdef MW2_ANDROID
        g_microphoneOpen = android::voice::Open();
#elif defined(MW2_USE_SDL)
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            LOGW("voice: SDL audio would not start (%s)", SDL_GetError());
            return;
        }
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_S16;
        spec.channels = 1;
        spec.freq = int(kVoiceRate);
        // SDL converts from whatever the device records at.
        g_microphone = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
        if (g_microphone)
        {
            SDL_AudioSpec device{};
            SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_microphone), &device, nullptr);
            SDL_ResumeAudioStreamDevice(g_microphone);
            g_microphoneOpen = true;
            LOGI("voice: microphone open, device %d Hz %d ch; read as 16 kHz mono 16-bit",
                 device.freq, device.channels);
        }
        else
            LOGW("voice: no microphone (%s)", SDL_GetError());
#endif
        if (!g_microphoneOpen)
            LOGW("voice: without a microphone the other players are still heard; this player is silent");
    }

    void CloseMicrophone()
    {
        if (!g_microphoneOpen) return;
        g_microphoneOpen = false;
#ifdef MW2_ANDROID
        android::voice::Close();
#elif defined(MW2_USE_SDL)
        SDL_DestroyAudioStream(g_microphone);
        g_microphone = nullptr;
#endif
    }

    uint32_t MicrophoneAvailable()
    {
#ifdef MW2_ANDROID
        return android::voice::Available();
#elif defined(MW2_USE_SDL)
        const int bytes = SDL_GetAudioStreamAvailable(g_microphone);
        return bytes > 0 ? uint32_t(bytes) / 2 : 0;
#else
        return 0;
#endif
    }

    uint32_t MicrophoneRead(int16_t* out, uint32_t samples)
    {
#ifdef MW2_ANDROID
        return android::voice::Read(out, samples);
#elif defined(MW2_USE_SDL)
        const int bytes = SDL_GetAudioStreamData(g_microphone, out, int(samples * 2));
        return bytes > 0 ? uint32_t(bytes) / 2 : 0;
#else
        (void)out; (void)samples;
        return 0;
#endif
    }

    // ---- capture -----------------------------------------------------------
    std::mutex g_lock;                    // the headset: open, the queue, the microphone
    std::condition_variable g_wake;
    bool g_open = false;
    bool g_pumpStarted = false;
    std::deque<uint32_t> g_capture;       // guest addresses of pending packets, oldest first

    void FillTone(int16_t* out, uint32_t samples)
    {
        static uint32_t at = 0;
        for (uint32_t i = 0; i < samples; i++, at++)
            out[i] = int16_t(8000.0 * std::sin(double(at % kVoiceRate) * (2.0 * 3.14159265358979 * 440.0 / kVoiceRate)));
    }

    // Hands one capture packet back to XHV, full. The status goes last: it is
    // the word XHV polls, from another thread.
    void Complete(uint32_t address, const int16_t* samples, uint32_t count)
    {
        auto* packet = GuestPtr<Packet>(address);
        auto* out = GuestPtr<be16>(packet->buffer);
        for (uint32_t i = 0; i < count; i++) out[i] = uint16_t(samples[i]);
        packet->size = count * 2;
        packet->done = count * 2;
        packet->format = kFormatPcm;
        __atomic_store_n(&packet->status.raw, be32::swap(X_STATUS_SUCCESS), __ATOMIC_RELEASE);
    }

    // Fills the queued capture packets as their samples exist, from the
    // microphone at the microphone's pace, or from the clock when there is
    // none (or the test tone stands in for it).
    void PumpMain()
    {
        crash::RegisterThread("voice capture");
        std::vector<int16_t> samples;
        Clock::time_point last = Clock::now();
        double owed = 0.0;                // samples the clock has made available
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::unique_lock<std::mutex> guard(g_lock);
            if (!g_open)
            {
                g_wake.wait(guard, [] { return g_open; });
                last = Clock::now();
                owed = 0.0;
            }
            const Clock::time_point now = Clock::now();
            const bool talking = Talking();
            if (!g_microphoneOpen)
                owed = std::min(owed + std::chrono::duration<double>(now - last).count() * kVoiceRate,
                                double(kMaxBacklog));
            last = now;

            while (!g_capture.empty())
            {
                const uint32_t address = g_capture.front();
                // What XHV asked for was a quarter of its buffer, in the wire
                // format; it gets the whole buffer of samples (see the top).
                const uint32_t count = GuestPtr<Packet>(address)->size * 4 / 2;
                if ((g_microphoneOpen ? MicrophoneAvailable() : uint32_t(owed)) < count) break;
                samples.assign(count, 0);
                if (g_microphoneOpen)
                {
                    // Read even when it is not sent, so that pressing the key
                    // starts with now and not with what was said before it.
                    MicrophoneRead(samples.data(), count);
                    if (!talking) std::fill(samples.begin(), samples.end(), int16_t(0));
                }
                else
                {
                    owed -= count;
                    if (talking && Tone()) FillTone(samples.data(), count);
                }
                Complete(address, samples.data(), count);
                g_capture.pop_front();
            }

            // XHV is not asking (no match, or it stalled): keep only the
            // newest of what the microphone has heard.
            if (g_microphoneOpen)
                for (uint32_t held = MicrophoneAvailable(); held > kMaxBacklog; )
                {
                    samples.resize(std::min(held - kMaxBacklog, kVoiceRate));
                    const uint32_t dropped = MicrophoneRead(samples.data(), uint32_t(samples.size()));
                    if (!dropped) break;
                    held -= dropped;
                }
        }
    }

    // ---- playback ----------------------------------------------------------
    std::mutex g_playLock;                // the ring below: XHV's thread writes, the mixer reads
    int16_t g_ring[kPlaybackRoom];
    uint32_t g_ringRead = 0, g_ringHeld = 0;
    bool g_playing = false;               // past the lead, until the ring runs dry
    float g_previous = 0.0f, g_next = 0.0f;
    uint32_t g_step = 3;                  // 48 kHz samples since g_next was taken; 3 takes another

    // MW2_VOICE_VOLUME, in percent of the level the title sent.
    float Volume()
    {
        static const float volume = float(std::min<uint64_t>(env::Number("MW2_VOICE_VOLUME", 100), 400)) / 100.0f;
        return volume;
    }

    void Queue(const int16_t* samples, uint32_t count)
    {
        std::lock_guard<std::mutex> guard(g_playLock);
        for (uint32_t i = 0; i < count; i++)
        {
            // Full means the mixer is not taking them (no sound device): the
            // oldest go, so what is heard when it does is recent.
            if (g_ringHeld == kPlaybackRoom) { g_ringRead = (g_ringRead + 1) % kPlaybackRoom; g_ringHeld--; }
            g_ring[(g_ringRead + g_ringHeld) % kPlaybackRoom] = samples[i];
            g_ringHeld++;
        }
    }

    // MW2_VOICE_DUMP=<file.wav>: what the other players said, as it was
    // decoded, to check the path end to end. The header is rewritten after
    // every packet, because a run ends by the process being killed.
    void Dump(const int16_t* samples, uint32_t count)
    {
        static const char* path = diag::Text("MW2_VOICE_DUMP");
        if (!path) return;
        static std::FILE* file = std::fopen(path, "wb");
        static uint32_t bytes = 0;
        if (!file)
        {
            static bool warned = false;
            if (!warned) { warned = true; LOGW("voice: could not write %s", path); }
            return;
        }
        bytes += count * 2;
        // The host is little-endian, as the file is.
        struct [[gnu::packed]] Header
        {
            char riff[4]; uint32_t riffSize; char wave[4];
            char fmt[4]; uint32_t fmtSize; uint16_t tag, channels; uint32_t rate, byteRate;
            uint16_t blockAlign, bits; char data[4]; uint32_t dataSize;
        } header{ { 'R', 'I', 'F', 'F' }, 36 + bytes, { 'W', 'A', 'V', 'E' },
                  { 'f', 'm', 't', ' ' }, 16, 1, 1, kVoiceRate, kVoiceRate * 2,
                  2, 16, { 'd', 'a', 't', 'a' }, bytes };
        std::fseek(file, 0, SEEK_SET);
        std::fwrite(&header, sizeof header, 1, file);
        std::fseek(file, 0, SEEK_END);
        std::fwrite(samples, 2, count, file);
        std::fflush(file);
    }

    // Guest memory for the coder: its state, and where it decodes to.
    constexpr uint32_t kDecodeRoom = 8192;            // bytes; a packet is 640
    uint32_t g_coderState = 0, g_decoded = 0;

    void ResetCoder()
    {
        if (!g_coderState)
        {
            g_coderState = kernel::AllocateGuest(T_XHV_AdpcmStateSize);
            g_decoded = kernel::AllocateGuest(kDecodeRoom);
        }
        // What the title starts its own coder with, for each headset it opens.
        if (g_coderState)
            std::memcpy(guest::Base() + g_coderState, guest::Base() + T_XHV_AdpcmState, T_XHV_AdpcmStateSize);
    }

    // One playback packet to host samples. False when it cannot be decoded.
    bool Decode(PPCContext& ctx, uint8_t* base, const Packet& packet, std::vector<int16_t>& samples)
    {
        const be16* decoded = nullptr;
        uint32_t count = 0;
        if (packet.format == kFormatAdpcm)
        {
            // Four bits a sample.
            count = packet.size * 2;
            PPCFunc* coder = kernel::GuestFunction(T_XHV_AdpcmCoder);
            if (!coder || !g_coderState || !g_decoded || count * 2 > kDecodeRoom) return false;
            // On this thread's guest stack, below the frame of the caller: the
            // import was reached by an ordinary call, so a callee may run there.
            PPCContext call = ctx;
            call.r3.u64 = 1;                          // decode
            call.r4.u64 = packet.buffer;
            call.r5.u64 = g_decoded;
            call.r6.u64 = count;
            call.r7.u64 = g_coderState;
            coder(call, base);
            decoded = GuestPtr<be16>(g_decoded);
        }
        else if (packet.format == kFormatPcm)
        {
            count = packet.size / 2;
            decoded = GuestPtr<be16>(packet.buffer);
        }
        else
            return false;
        samples.resize(count);
        for (uint32_t i = 0; i < count; i++) samples[i] = int16_t(uint16_t(decoded[i]));
        return true;
    }
}

void kernel::voice::Mix(float* interleaved, uint32_t frames, uint32_t channels)
{
    if (!Enabled() || channels < 2) return;
    std::lock_guard<std::mutex> guard(g_playLock);
    if (!g_playing)
    {
        if (g_ringHeld < kPlaybackLead) return;
        g_playing = true;
    }
    const float volume = Volume();
    for (uint32_t i = 0; i < frames; i++)
    {
        // 16 kHz to 48: three output samples along the line between two.
        if (g_step == 3)
        {
            if (!g_ringHeld) { g_playing = false; return; }
            g_previous = g_next;
            g_next = float(g_ring[g_ringRead]) / 32768.0f;
            g_ringRead = (g_ringRead + 1) % kPlaybackRoom;
            g_ringHeld--;
            g_step = 0;
        }
        const float sample = (g_previous + (g_next - g_previous) * (float(g_step) / 3.0f)) * volume;
        g_step++;
        interleaved[size_t(i) * channels] += sample;
        interleaved[size_t(i) * channels + 1] += sample;
    }
}

// XamVoiceCreate(user, flags, HANDLE* headset). With voice chat off, and for
// any player but the first (there is one microphone), this is what it was
// before there was voice chat: success with no handle written, which XHV takes
// as "no headset" and fails its own capture packets without asking again.
PPC_FUNC(__imp__XamVoiceCreate)
{
    const uint32_t user = ctx.r3.u32;
    auto* handle = GuestPtr<be32>(ctx.r5.u32);
    ctx.r3.u64 = 0;
    if (!Enabled() || user != 0 || !handle) return;
    std::lock_guard<std::mutex> guard(g_lock);
    g_capture.clear();
    ResetCoder();
    OpenMicrophone();
    g_open = true;
    if (!g_pumpStarted)
    {
        g_pumpStarted = true;
        std::thread(PumpMain).detach();
    }
    g_wake.notify_all();
    *handle = kHandle;
    LOGI("voice: headset open for player 1; %s, %s", Tone() ? "a test tone for a microphone"
             : g_microphoneOpen ? "microphone" : "no microphone",
         !PushToTalk() ? "always sending"
#ifdef MW2_ANDROID
             : "sending while the Talk button is held");
#else
             : "sending while T is held");
#endif
}

PPC_FUNC(__imp__XamVoiceHeadsetPresent)
{
    std::lock_guard<std::mutex> guard(g_lock);
    ctx.r3.u64 = (g_open && ctx.r3.u32 == kHandle) ? 1 : 0;
}

// XamVoiceSubmitPacket(headset, direction, packet). Negative fails the packet;
// XHV then completes it itself, as silence.
PPC_FUNC(__imp__XamVoiceSubmitPacket)
{
    const uint32_t direction = ctx.r4.u32, address = ctx.r5.u32;
    auto* packet = GuestPtr<Packet>(address);
    if (ctx.r3.u32 != kHandle || !packet)
    {
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }
    // The first packets each way, once, with what the top of this file takes
    // for granted about them.
    static uint32_t said[2] = {};
    if (direction < 2 && said[direction] < 3)
    {
        said[direction]++;
        LOGI("voice: %s packet %08X: %u bytes at %08X, format %u (expected: format 1, and 160 bytes for 20 ms)",
             direction == kCapture ? "capture" : "playback", address, uint32_t(packet->size),
             uint32_t(packet->buffer), uint32_t(packet->format));
    }
    // A packet is 20 ms; one far larger is not a packet.
    if (!packet->buffer || !packet->size || packet->size > kDecodeRoom / 4)
    {
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    if (direction == kCapture)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (!g_open) { ctx.r3.u64 = X_STATUS_UNSUCCESSFUL; return; }
        packet->status = X_STATUS_PENDING;
        g_capture.push_back(address);
        ctx.r3.u64 = 0;
        return;
    }
    if (direction == kPlayback)
    {
        static std::vector<int16_t> samples;      // XHV's one worker thread submits
        if (Decode(ctx, base, *packet, samples))
        {
            Queue(samples.data(), uint32_t(samples.size()));
            Dump(samples.data(), uint32_t(samples.size()));
        }
        else
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                LOGW("voice: a playback packet of %u bytes in format %u could not be decoded; it is not heard",
                     uint32_t(packet->size), uint32_t(packet->format));
            }
        }
        // Played or not, the headset is done with it at once: the sound
        // output keeps the pace, not this queue.
        packet->done = uint32_t(packet->size);
        packet->status = X_STATUS_SUCCESS;
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
}

// XamVoiceClose(headset): the packets still queued are XHV's again.
PPC_FUNC(__imp__XamVoiceClose)
{
    std::lock_guard<std::mutex> guard(g_lock);
    if (ctx.r3.u32 == kHandle && g_open)
    {
        g_open = false;
        g_capture.clear();
        CloseMicrophone();
        LOGI("voice: headset closed");
    }
    ctx.r3.u64 = 0;
}

#else

// A title whose voice library has not been looked at: no headset, and the
// answers these imports gave before there was voice chat.
void kernel::voice::Mix(float*, uint32_t, uint32_t) {}

PPC_FUNC(__imp__XamVoiceCreate)         { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XamVoiceHeadsetPresent) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XamVoiceSubmitPacket)   { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XamVoiceClose)          { ctx.r3.u64 = 0; }

#endif
