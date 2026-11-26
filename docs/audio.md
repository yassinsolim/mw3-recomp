# Audio

The console's sound is split in two. The mixer is XAudio2, statically linked
into the title and therefore recompiled guest code. What it needs from outside
is a **render driver**, which calls its mix callback every 256 samples at
48 kHz (5.333 ms) and takes the six channels it hands back, and the **XMA
decoder**, hardware that XAudio2 drives through memory-mapped registers for
every compressed voice. Both are in `runtime/apu/`; the kernel imports that
reach them are in `runtime/kernel/audio.cpp`.

## Render driver (`apu/audio.cpp`)

`XAudioRegisterRenderDriverClient` receives `{ callback, context }` and returns
a driver handle. A worker thread, with its own guest stack (1 MB) and thread
block like any guest thread, calls `callback(context)` on the 5.333 ms cadence,
starting 100 ms after registration so the title has finished setting up its
object. Inside the callback the title mixes and calls
`XAudioSubmitRenderDriverFrame` with six planes of 256 big-endian floats (FL FR
FC LFE BL BR). These are interleaved and pushed to an SDL3 audio stream opened
as 5.1 float at 48 kHz; SDL converts to whatever the device is.

The callback receives the context value itself, not a pointer to it: MW2's
callback tests a "started" flag inside that object and returns without mixing
when it is clear.

Pacing is locked to the wall-clock 5.333 ms cadence (187.5 Hz) matching the exact
48 kHz sample production rate. Buffer headroom allows up to 48 queued frames (256 ms)
to withstand dense explosions or level transitions before frames are dropped. The callback
keeps its nominal rate, advancing the sound state machines smoothly without pitch artifacts
or underruns. A worker that falls far behind (the process was suspended: 40 frames, 213 ms)
takes the clock afresh instead of pumping a burst. `MW2_NO_AUDIO=1` opens no device; the mixer
and the decoder still run.

On Android a frame that comes late (the mixer's thread kept off its core while a
level starts or pipelines compile) is mixed all the same, and the frames due since
straight after it. The title's sound runs on the count of these frames: a
streamed sound is as far along as the frames mixed since it started, so a frame
passed over left every sound then playing that much behind the picture until it
ended, and a cutscene's sound track is one sound. The device played silence
meanwhile, so the frames mixed late are not played once the sink holds what it
does when the mixer is on time (what the device takes at once and a frame
more): played as well they would stay in the sink, and everything after them
would be later for good.

The device plays on its own clock, not the one the frames are timed on, and
what the mixer makes beyond what the device plays stays in the sink for good.
At 5333 microseconds a frame, 62 samples in a million too many, the sink grew
3.3 ms a minute on the Retroid Pocket Flip 2 and 3.7 on the AYN Thor (4 to
41 ms in ten minutes), up to the 170 ms it holds: the sound of everything late
by that, speech against lips first, and a Bink movie's with it, since the title
times a movie's pictures to the sound it has handed over. The interval is
5333333 nanoseconds now, and on Android the mixer follows the device: half a
hundredth slower while the sink holds more than it does when both are on time,
as much faster while it is empty when a frame is due (6.7 ms held after ten
minutes on the Thor, as at the start). `MW2_TRACE_MOVIES=1` logs both clocks
and the sink ([switches.md](switches.md)).

What the device adds is its own: 22 ms on the Retroid Pocket Flip 2's low-latency
output, 46 ms on the AYN Thor's, and 133 ms on the Thor's ordinary output, which
the system gives a stream while an effect on everything played (an equaliser
application; JamesDSP there) is on, and for Bluetooth. The log's `audio:` line
says which the stream got, with a warning for the ordinary one.

Two other imports matter to the mixer:

- `XAudioGetVoiceCategoryVolume` reports full volume, since there is no
  dashboard.
- `XAudioGetVoiceCategoryVolumeChangeMask` reports every category changed on its
  first call. The mixer only fetches a category's volume when told it changed,
  and its object starts zeroed, so otherwise every voice stays at volume 0.

The mixer also depends on kernel behaviour described in
[runtime.md](runtime.md): it creates its mixing thread suspended and starts it
with `KeResumeThread`; its workers wait for each other by processor number,
which comes from the top byte of `ExCreateThread`'s flags; its callback waits
on a "done" and a "stop" event with `KeWaitForMultipleObjects` and needs the
index back; and it reads `KeDebugMonitorData`, which must point at a zero word.

## XMA decoder (`apu/xma.cpp`)

A port of Xenia's context decoder (`xma_context_new.cc`): the same context
layout, the same packet and frame walk. XMA2 is WMA Pro in 2 KB packets, each
starting with a 32-bit header and holding 15-bit-length-prefixed frames of 512
samples that may straddle packets. The context records a bit offset into its
input buffer, and XAudio2 loops a sound by having the decoder jump from the loop
end back to the loop start.

**Contexts.** 320 contexts of 64 bytes are allocated from the physical heap
before the title starts (not at physical address 0, which reads as "no
context"), since the title finds a context's hardware index from its physical
address. `XMACreateContext` and `XMAReleaseContext` hand them out.

**Registers.** The register window at `0x7FEA0000` is plain guest memory.

| offset | direction | contents |
|---|---|---|
| `+0x1800` | read | physical address of context 0 |
| `+0x1818` | read | the "current context", preset to one no context has, so the title's lock-all never sees a busy context |
| `+0x1940` | write | kick: ten words, one bit per context |
| `+0x1A40` | write | lock |
| `+0x1A80` | write | clear |

The written registers are commands, not state, so they are caught as they are
stored. The recompiler emits `PPC_MM_STORE_U32` for a store followed by
`eieio`, and `runtime/mmio_hook.h`, force-included into the recompiled code,
routes stores into the window to `mw2_xma_register_store`. A kick decodes
synchronously on the kicking thread until the output ring is full or the input
is spent, so the context reflects the work when the store returns. A lock waits
out a decode in flight on another thread.

**Frames** are decoded by the `xmaframes` codec from Xenia's FFmpeg branch
(`has207/FFmpeg`), fetched as a tarball at configure time and built as the
subset in `third_party/ffmpeg-xenia/`, with Xenia's hand-written `config.h`.
Stock FFmpeg has only a packet-level XMA2 decoder, which cannot follow the
frame-precise bit offsets and loop points a context works in.

## Diagnostics

The end-of-run report (diagnostic builds) gives, for the driver, callbacks,
frames submitted, drops, early pumps and whether the device is open; for the
decoder, kicks, locks, clears, frames decoded, the contexts used and the peak
allocated, and errors.

| switch | effect |
|---|---|
| `MW2_NO_AUDIO=1` | no playback device |
| `MW2_TRACE_XMA=1` | every kick with its buffers, and FFmpeg's log |

To check the output without listening, record the sink's monitor while the game
runs:

    parec --device="$(pactl get-default-sink).monitor" --file-format=wav out.wav
