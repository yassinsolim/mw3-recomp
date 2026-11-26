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

The mixer does not wait for the picture: its thread has its own cadence, and
the decoder is driven from it and from XAudio2's own threads, none of them
the title's frame loop. So a frame rate that is low because the graphics chip
is slow leaves the sound alone -- an AYN Thor drawing Advanced
Warfare's Seoul at three times its size ran at 20 to 49 frames a second with
no underrun in play but at the level's start, and no frame dropped -- and what makes it choppy
is a processor with more to do than it has cores for, which runs the mixer's
thread and the title's audio threads it waits for tens of milliseconds late,
several times a second. The mixer then mixes the frames it owes in a burst;
the sink took of them what it holds when the mixer is on time (what the
device takes at once and a frame more) and the rest were dropped as owed to
nobody, and the device played silence until the next burst. On the Thor with eight busy
shells beside the game (25 to 50 frames a second, the processor 97% busy,
80% of it with other programs): 152 underruns in 82 s of play and 224 frames
dropped.
Three things answer it, all under Android (`runtime/android/audio.cpp`,
`MixedLate`; `android::perf::RaiseAudioPriority`):

- **The sink holds as much more as the mixer has been late.** Every frame's
  lateness is taken when its callback returns; frames late one after another
  are a spell, and the sink's cushion grows by the third longest spell of
  this quarter-minute or of the one before, up to 160 ms (the ring is 256 ms
  now, where it was 170). The third, because one or two long spells are a
  level starting, and holding the sound back by them for the half minute
  after would cover a pause that is over: the cutscene that follows is then
  as near its picture as before. A burst now fits the sink, and the mixer
  runs a fiftieth fast while the sink holds two frames less than the cushion.
  When the lateness stops the cushion goes within half a minute, and the
  mixer's half a hundredth slow takes the sound back to where it was.
- **The mixer's thread runs at Android's audio priority** (nice -16; it had
  the default), and so does any thread that drives the decoder
  (`apu/xma.cpp`, at its first kick), which is how XAudio2's own threads are
  told from the title's.
- **The log says so** (`audio: the mixer is finishing its frames late (the
  processor is busy); the sink holds 80 ms when it is on time`, and `on time
  again`), and the performance report has the underruns and the cushion.

Measured on 26 November 2026, `.dev` builds, Advanced Warfare's Seoul with
eight busy shells beside the game. On the Retroid Pocket Flip 2, which holds
59 frames a second under that load: 12 underruns in 110 s of play where the
build before had 38, 3 frames dropped for 9. On the Thor, where the run is
held to 60 and then to 30 under it with the display kept at 120 Hz (below):
1 underrun in 106 s of play and 134 frames dropped, where the release build
had 152 in 82 s and 224; and 3 underruns in 256 s of play in a run whose
load ended after 110 s. With the graphics chip as what is slow (the internal
render multiplier at 2x and 3x, 15 to 49 frames a second) the Flip 2 had 11
underruns in play and the Thor 2, a frame dropped at most, before and after
alike; the Thor at 3x on the final build had none.

**The display's slower mode was most of it on the Thor.** The first runs of
the cushion there were far worse than the build before: 2,167 underruns in
108 s of play, the mixer making 60 to 75% of the sound in time and stopping
for 300 to 500 ms at a time. (A first form of the cushion also dropped 2,801
frames, which was its own: it left out every lateness over 200 ms as a
stall. The rule above counts every lateness.) It began ten seconds after the
rule that follows the device had held the run to 60 and the display had gone
to its 60 Hz mode, in three runs of three, and the release build does the
same with an FPS limit of 30 chosen: 2,427 underruns a minute of play. In
its 60 Hz mode the Thor's system gives the game 8 to 9% of the processor
under that load, where it has 16 to 17% at 120 Hz. The busy shells are in
the root control group and the game in `top-app`, each with the same weight
on a processor thread, so the mixer's priority counts among the game's own
threads and no further; what the display's mode changes there was not found
(the processors' clocks are at their fastest and the same cores are active
in both). So the display's mode is chosen with the sound in mind
([android.md](android.md#what-a-device-is-given-and-what-it-is-measured-for)):
it keeps its fastest where the processor is what is late, and is put back
there, a limit the player chose or not, once the sound has broken up in a
slower one. With a limit of 30 chosen under that load the display was back
at 120 Hz sixteen seconds into the run, during the loading and 61 underruns
after they began, and the play had 7 in 67 s.

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
