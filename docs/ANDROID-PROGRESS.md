# Android port progress log

Status words: **verified** (observed on the device or by a run), **built**
(compiles/links only), **not started**.

| System | Status |
|---|---|
| Disc inspection (format, XEX identity, asset inventory) | verified |
| Thor detected over ADB, hardware inventory | verified |
| Architecture selected | done, see [ANDROID-ARCHITECTURE.md](ANDROID-ARCHITECTURE.md) |
| Recompilation of this disc's `default.xex` | verified: 128 jump tables, 0 boundary errors, no unrecognised instructions |
| Desktop reference (x86-64 Linux, llvmpipe) | verified: start-up, first-run setup, main menu and CAMPAIGN menu render, 1280x720 captures |
| Android arm64 build (`assembleMw3Tu0Debug`) | verified: debug APK (27 MB with the bundled driver), installs on the Thor; builds natively on Windows as well as in WSL. `setup/setup.ps1` is the Windows build in the repository (owner-independent paths, tools pinned by SHA-256); it is the same sequence as the owner's earlier `scripts/win_build.ps1` but has not yet run end to end on a clean PC |
| On-device install from the ISO | verified: title id and hash check pass, 184 files copied |
| Guest code executing on the Thor | verified: start-up, menus, Survival and the campaign (Black Tuesday) run |
| Graphics driver | **Turnip v27.0.0-R2 is the default** on Qualcomm devices (bundled, downloaded at build time). On the Thor's campaign helicopter scene: 51 FPS and clean textures on Turnip against 21 FPS with blocky, missing texture blocks on the stock Qualcomm driver. On Turnip, whole pipelines are built, not pipeline libraries (its compiler faulted building libraries) |
| Menus on the Thor | verified: title, first-run setup, main menu, Special Ops, Survival map list at 60 FPS; no flicker (black frames 0% in a 7 s recording, was 40%) |
| Survival on the Thor | **60 FPS nearly locked**: Resistance loads in about 15 s; everything drawn and textured. On Turnip R2, 10 minutes kept in play: 59.1 world frames a second on average, 87.6% of frames within 17.5 ms, 289 of 325 sampled seconds at 59-60 and the worst at 53; GPU 79% busy (13.2 ms a frame). Temperatures flat over the 10 minutes (hottest CPU zone 88-90 C, GPU 80-81 C) |
| Campaign on the Thor | Black Tuesday plays at **58.2 world FPS** over 137 s of play (87.7% of frames within 17.5 ms, GPU 73% busy, 12.6 ms a frame); fires, smoke and the rifle scope drawn; no fault |
| Enemies, AI, damage | observed: enemies spawn, approach and shoot, armour and health fall, last stand triggers |
| Player movement, aiming, shooting | verified with the touch overlay (move, look, fire); a wave played by hand by the owner |
| Stability | Turnip with pipeline libraries crashed in its compiler (twice), so Turnip builds whole pipelines; 11-minute Survival runs without fault. Turnip T26 crashes at renderer start-up; T30, V32, faux123 and Banners builds run (all within 3% of R2) |
| Audio on the Thor | verified by ear by the owner |
| Input on the Thor | touch overlay verified in menus and play (a press must be held about 0.25 s); physical pad detected, not exercised |
| Launcher branding | verified: MW3 wordmark, MW2-only entries hidden |
| MW3 multiplayer on the Thor | **booting and playable locally**: `iw5mp_ship.exe` recompiled (app `com.mw3.recomp.mp`); Split Screen Team Deathmatch on Seatown at 60 FPS. 15 of MW2's 18 network fixes located and wired, 3 absent in IW5, 2 IW5-only bugs fixed (commit 5a308a8); a local match plays through them unchanged. **Everything unlocked** (MW2_UNLOCK_ALL, default on): Create a Class at rank 1, every weapon at level 41, all attachments and proficiencies, all camos up to Gold; the save is not written. Networking still off (loopback only) until LAN play is built and tested. No bots on the 360 build |
| Skins | texture replacement by content key (`replace/<key>.png`, dumps named by key), verified on the multiplayer menu art; **animated** replacements (`<key>.N.png` + fps) verified cycling at 6 fps. Tools: `camo_gen.py` (original procedural camos), `iwi_match.py` (a player's own PC .iwi mods mapped onto this port's keys). No third-party art bundled |
| Online (MW3 multiplayer) | verified 2026-11-18: **System Link over the public relay** `mw3.yassin.app` between the Thor and the PC build (PC listed, joined and loaded the Thor's Seatown match); **XBOX LIVE menu** on the Thor through the in-runtime DemonWare stand-in: signs in, FIND MATCH lists TDM/FFA/Domination/Kill Confirmed, a TDM search opens a public lobby ("Waiting for 1 more player"); two PC copies matched each other through Find Game (runs by agent LiveImpl). Not yet: a full Live match end to end, idle lobby beyond 2 min, leaderboards |
| Bots | verified on the Thor: `MW2_BOTS=4` (app setting "Bots in my matches") joined, spawned and scored in a System Link match at 60 FPS. A bot dropped with "Server command overflow" after ~40 s; fixed (reliable commands acknowledged), 180 s PC run with 6 bots had no drop. Re-run on the Thor 2026-11-20: six bots through a ten-minute Seatown match, the vote and three and a half minutes of Lockdown, none dropped. Navigation is local only |
| Camos in a match | verified on the Thor: Gold chosen in Create a Class now spawns gold (the spawn check's weaponRank read was not covered before); Gold replaced by an original animated camo on the weapon in a match (keys in docs/textures.md) |
| Voice chat | implemented (XamVoice*, PC SDL capture + T push-to-talk, Android AAudio + Talk button, off by default); compiled only, not run |
| PC build | mouse captured in the window (F2 frees it), keyboard works in menus; mouse capture not yet tried by hand |
| Profile and saves | profile and campaign saves written and resumed |

## 2026-11-16

- Inspected the ISO read-only: XGD3, title `415608CB`, `iw5sp_ship.exe` /
  `iw5mp_ship.exe`, base version, 16 Survival zones present.
- Thor: QCS8550, Android 13, Vulkan 1.3, 16 GB RAM, 4 KiB pages.
- Surveyed XenonRecomp, mw2-recompiled, the mw2-android fork, ReXGlue,
  MW32011NCP and T6Requiem; no existing MW3 Android port found.
- Repository created from `the-drk/mw3recomp` history (GPL-3.0).
- This disc's `default.xex` has a different file hash from the upstream-pinned
  one but recompiles with the same config; the desktop headless run gives
  1829 frames in 30 s with no unknown GPU opcodes and no unimplemented import
  reached. Both hashes are accepted.
- Merged the Android layer of `flavinxs0/mw2-android` (33 conflicted files).
  Left out its committed recompiled MW2 code, bundled MW2 shader cache and
  game-downloading CI workflows. The desktop MW3 build still links and runs.
- Added the `mw3` app flavor (`com.mw3.recomp`), MW3 disc identity in the
  installer, and regenerated the Vulkan loader table.
- First device run: installed from the ISO on the Thor, the recompiled ARM64
  code runs the start-up sequence at 60 FPS (HUD counter), 21-26% CPU, 47 C.
- `stfsux`/`stfdux` are now translated; AArch64 gets real barriers for
  `lwsync`/`sync`/`eieio` and the correct host-to-guest rounding table.
- Found why every menu was drawn over the last one (upstream's blocker): MW3
  clears its interface with D3D's Clear, which on Xenos is a depth-only
  rectangle over the colour surface's own EDRAM tiles; only the depth side was
  honoured. With the colour side cleared the first-run setup completes. Each
  setup screen needs D-pad left/right before A is accepted.
- Thor: title -> setup -> main menu -> Special Ops -> Solo Play -> Survival ->
  Resistance. The map loads and the game is at wave 1, but the world is not
  drawn. That is the next blocker.
- The merge had also changed shared code outside any Android guard: the PM4
  parser and `WAIT_REG_MEM`, physical-address resolution, arena progress being
  forced by a hang monitor, frame-fence give-up, and on Android draws being
  skipped while their pipeline compiled. An audit listed 18 such changes; they
  are reverted or confined to the MW2 phone build. The asset-free tests build
  again and pass (16 of 16).
- With that, Survival draws its world on the Thor and enemies fight back.
  Next: the rendering defects, frame rate, and the abort above.
- The black sky and street on the Thor: ruled out, each by a device run, are
  the texture budget (was 128 MB, now device-aware, 1536 MB), software BC
  decoding (`MW2_FORCE_BC_HW=1`), the upload arena (was forced to 144 MB, now
  384 MB; frame peak is 32 MB), pipelines still compiling, and the depth
  prepass (skipping every depth-only draw leaves them black). The desktop
  build with the same switches, and with the Android-only caps compiled in,
  draws them. The device's end-of-run report shows no failed textures, no
  fallbacks and no skipped draws beyond the three known reasons. Vertex
  positions are now `Invariant` with `NoContraction` arithmetic, which is
  right for a prepass but did not change the picture. Not yet tried: a frame
  capture on the device, depth-texture sampling for the sun shadow, and the
  sky's own technique.
- In play the title sends about 6,500 draws a frame; the ring consumer is
  busy 43% of the run. (Corrected 2026-11-17: these are not two EDRAM tiles
  replayed. A census found every draw unpredicated, window offset 0.)

## 2026-11-17

- The flicker and the black sky/streets had one cause: the renderer binds its
  frame arena and the guest-memory shadow as one storage buffer, and the
  Thor's Adreno 740 reaches 128 MB with one; the buffer was 384 + 512 MB.
  The third frame slot's share (every third frame black) and the whole shadow
  (vertex data for much of the world) were past the reach. The arena is now
  clamped to the reach and the shadow left out when it does not fit. Cost:
  every draw copies its vertices again. Restoring the shadow in a form that
  fits (its own binding, or buffer device addresses) is the next performance
  job.
- Shader registers start at zero (they were uninitialised SPIR-V variables).
- Turnip v27.0.0-R2 bundled and selected by default on Qualcomm devices.
- WSL stopped responding (it needs an administrator to restart), so the APK
  is now built natively on Windows with portable tools under
  `mw3-android-work\toolchain\`. Its debug key differs from the WSL one, so
  the app had to be reinstalled once; game files and saves were moved aside
  and restored.
- Each frame slot now has its own arena descriptor set, bound at the slot's
  base, so each slot (not all three together) has to fit within the 128 MB
  storage-buffer reach. Survival had skipped 35,895 draws in 150 s for a full
  arena; now none.
- The write watch on guest memory ran only together with the shadow, so on
  the Thor every texture check hashed the texture's bytes again: 33.6 s of a
  150 s Survival run on the ring consumer. The watch now runs without the
  shadow: 2.9 s, and 4.3 million checks answered by it. World frame rate is
  unchanged at about 22 a second; the consumer now mostly finishes a frame in
  under 12 ms of CPU and waits on the GPU (49 s of 150 s), so the next limit
  is GPU work, which has not been profiled yet.
- GPU profile of Survival on Turnip (MW2_REPORT=1, MW2_TIME_PASSES=1): the
  chip is busy 87-98% of play, about 40 ms a world frame, 86% of it in the
  passes on the 1040x2048 scene surface (about 63 passes and 6,600 draws a
  world frame). Ruled out, each by a device run: render area (passes cut to
  their draws' scissors: no change, reverted). The first TU_DEBUG runs never
  reached the driver (.env passes only MW2_ switches); MW2_DRIVER_ENV (new)
  does, and then: TU_DEBUG=sysmem 39.6 ms (Turnip already draws this frame
  direct to memory), TU_DEBUG=gmem 64.9 ms, IR3_SHADER_DEBUG=nopreamble
  38.8 ms. What is left is the draws' own work, the
  translated shaders or per-draw cost. Split with temporary switches
  (removed): every draw with zero instances takes the GPU to 3.7 ms a frame
  and the game to 59.8 world frames a second, so the CPU side can already
  reach 60; an empty scissor on every draw (vertices only, no pixels) still
  costs 35.4 ms; and every draw cut to one triangle (draws and state kept,
  vertices gone) still costs 34.6 ms. **Survival is bound by a fixed GPU
  cost per draw**, about 5 us across some 6,600 draws a world frame, not by
  vertices or pixels. Not the register zeroing (39.8 ms with or without).
  MW2_SHADER_STATS=1 (new, kept): the busiest vertex shader is 90
  instructions, 12 registers, no loads in its per-vertex body; its 200-260
  instruction preamble reads the draw's constants through a chain of
  dependent arena loads (record, table, block copy, value), which every draw
  pays before its first vertex. Not the cause: no preambles cost the same,
  and one base vertex for a whole multi-draw run (no per-draw parameter
  change) 38.3 ms.
- Draws in one in-play frame by target (census, removed): 4,861 at 1024x600
  on the lit scene's target, about 900 at 1024x600 on a colour-masked target
  (the depth prepass, by its mask), 703 at 1024x1024 (the shadow map), about
  250 others. All unpredicated: the title's own, not tile replays.
- The stock Qualcomm driver in the same Survival run: GPU 58.1 ms a world
  frame, 16.1 world FPS (Turnip R2: 38.9 ms, 25.3). The large fixed cost per
  draw is there on both drivers, so it is in how this renderer drives the
  Adreno, not in Turnip.
- Hardware counters: Turnip on the Thor offers no VK_KHR_performance_query,
  with or without TU_DEBUG=perfc, so a counter diagnostic was written,
  found unusable here and removed.
- Drivers, same Survival run (GPU ms a world frame / world FPS): faux123
  v1.2.10 38.7/25.5, v27.0.0-R2 38.9/25.3, Banners 2026-11-17 39.2/25.2,
  mainline V32 39.4/25.0, MrPurple T30 39.8/24.8. All within 3%; R2 stays
  the default. A driver cannot buy the factor of two the per-draw cost needs.
- Render scale 2 is not hung: its first launch compiles about 624 pipelines
  (72 s) before play, which the hang detector reports as hangs. It then
  plays at 18 FPS (GPU 54.5 ms a frame).
- Draw merging (commit bfc3ace): consecutive draws that change no state are
  one vkCmdDrawIndexed over 32-bit indices encoding a draw-table slot;
  Survival 25.3 -> 53.0 world FPS, GPU 38.9 -> 15.3 ms a frame.
- Ring consumer CPU (commit a47f6cc), guided by simpleperf on the device:
  batched recorder publication, lock-free heap coverage answers in
  FromPhysical, a fetch-constant cache in front of the texture map and the
  rewritten texture's bind-time re-read deferred to its segment's end.
  53.0 -> 59.8 world FPS in a short run; the consumer 65% -> 48% of a core.
- 10 minutes kept in play (Y every 20 s restarts after a game over): 58.5
  world FPS average, dips to 46-58 in heavy moments with the GPU 90% busy.
  A first 11-minute run that left the game-over screen up most of the time
  read 59.9 and is not counted as play.
- The GPU's busy time is 98.5% the renderer's frames, 1.5% the copies to
  the screen (new report split). With no pixel shaded the GPU took 8.1 of
  15.0 ms: merging had doubled the pixel cost (about 4 -> 7 ms), because the
  pixel stage read its record and indexed textures per pixel. It now takes
  one record at a time across the wave (a loop over the records a wave
  holds; one pass in nearly every wave), with uniform texture handles:
  GPU 15.0 -> 13.2 ms a frame, and 10 minutes in play at 59.1 world FPS
  (87.6% of frames within 17.5 ms, worst second 53).
- Tried and reverted, no measured change: occlusion-query brackets kept
  inside the open render pass (GPU 15.5 ms against 15.0).
- Build PC: two unrelated python servers hold most of the PC's memory; the
  Gradle daemon's heap is lowered in scripts/win_build.ps1 (outside the repo)
  so builds start beside them.
- Frame pacing over those 10 minutes: the window showed every frame (none
  let go, none held). 131 stutters in 654 s of play lost 370 of about
  39,000 frames (0.95%); 114 of them one frame. 107 were the renderer late,
  not the title. Most of the "17.5-25 ms" share is ordinary vsync jitter
  (19,914 frames 16.7-20 ms swap to swap; 682 over 20 ms). What is left is
  texture churn: 68,910 textures (8.3 GB) uploaded in the 11 minutes and
  67,439 released because the title's image pool rewrote their memory, read
  and decoded on the ring consumer at segment ends. Moving that decode off
  the consumer is the next step for the remaining hitches.
- Texture re-reads at a segment's end now run fork-join: the guest-memory
  read, endian swap, BC decode and emptiness measure for each image go to up
  to 3 reader threads (MW2_TEXTURE_THREADS) while the consumer waits, then
  staging is recorded on the consumer in the old order. 10 minutes in play:
  stutters 131 -> 66 (6.1 a minute, 50 of them one frame), renderer-late
  107 -> 49, 59.1 world FPS, no fault. The few long stutters left (5 of 16+
  frames) come with the restarts the test forces after each game over.

## 2026-11-19

120 FPS on the Thor's 120 Hz display (FPS limit 120, Turnip R2), measured
with 150 s runs, play from about 65 s.

- The ring consumer has the fastest core to itself (`gpu.cpp`,
  `platform::ThreadAffinityRole::PrimeCore`) and the title's main thread is
  with its other threads on the performance cores (`main.cpp`). The main
  thread was pinned to the one prime core and every thread started from it
  took that with it: the consumer, the presenter, the guest's blank and the
  audio shared one core, the consumer got 63% of it and was "at work" 96% of
  the time. Survival 61.7 -> 74.4 world FPS (frames within two display
  periods 67% -> 94%). The multiplayer on Seatown, whose spawn differs from
  run to run: 92-100 in play in five runs of seven with 36-58% of frames
  within one period, 115-119 every second where the spawn looks indoors,
  76 at the plaza and 68 at the yard (67 in the one run before the change).
- D3D's wait for arena space is made asleep (`gpu::AwaitArenaProgress`,
  `arena.cpp`) before the title's own wait, which spins: by simpleperf half
  of the render thread's processor time was `sub_8209B5E8` and the delay
  loop it calls. Three to five waits a frame are longer than 100 us. The
  render thread 66% -> 34% of a core; the frame rate does not move, since
  the thread was waiting either way.
- What holds Survival at 74-76 FPS is the GPU: 12.5-12.8 ms a frame at
  680 MHz, 94-95% busy. By temporary switches (removed): 4.3 ms of it is
  pixels (every scene draw scissored to one pixel: 8.6 ms), about 2.7 ms the
  shadow maps and depth prepass, about 2.6 ms what is left with no scene
  draw at all (the 1280x720 passes, resolves, copies, uploads). The draws
  are 2.45 million indices a world frame, in about 650 hardware draws; 96%
  of the runs end at a pipeline change. The game with no scene draws holds
  119 FPS, so the title's own threads and the parse are not the limit.
- Tried and not kept: `TU_DEBUG=gmem` 20.8 ms a frame and `noubwc` 14.8
  (12.5 without); `MW2_ADRENO_BC=hw` 12.4 ms and the same picture in two
  screenshots, with a third of the texture memory (96 MB against 326), not
  made the default for 2% without a soak for the flicker textures.md
  describes; a pixel program of no instructions for depth-only draws 12.4
  (the title already loads a small one); one command buffer and render pass
  across the segments that upload nothing 11.9 ms (-0.6) but not safe as
  tried, and the safe rule -- ending the buffer at any segment that binds a
  rewritten texture -- ends it at 78% of them and gains nothing; a frame
  queued behind the one the GPU is drawing, no gain and a deeper queue.
- Not found: the multiplayer outdoors (the plaza and the yard spawns) runs
  57-80 FPS with the GPU 60-80% busy and the consumer at half a core, so
  neither is the limit there; the spawn is random and no run was traced at
  one.
- Heat: the hottest zone is at 93-96 C within a minute of play and stays
  there; in a 7-minute run (77 s of play, then the game-over screen, which
  still draws the world) the GPU held 680 MHz and 76-77 FPS throughout. A
  run started with the zones at 80 C ran at 475-550 MHz and 45 FPS. A fifth
  of the processor's time in play is other programs: the launcher shell on
  the lower screen plays video (30% of a core, and its layer is composed by
  the GPU), SurfaceFlinger and the composer 20% and 17%.

## 2026-11-20

Modern Warfare 2 at 120 FPS on the Thor (FPS limit 120, display-paced,
Turnip R2, `.dev` packages, the launcher's performance mode with the fan on;
the standard mode gave the same numbers: 102.1 and 102.7 against 102.0).
S.S.D.D. is the recorded flight (`tools/routes/ssdd-flight.txt`), its
opening the first 18 s of the level, which is the heaviest view found.

| | before | after |
|---|---|---|
| S.S.D.D. flight (66 s), FPS | 102.0 | 108.5 |
| S.S.D.D. opening, FPS / GPU ms a frame | 84.7 / 11.5 | 93.1 / 10.4 |
| S.S.D.D., five minutes (flight, then its last view) | 114.9, 83.9% of frames within 1.05 periods, 7.7 ms (joins off) | 115.9, 85.0%, 7.1 ms |
| Terminal, walked, five minutes | 107.1, 79.4% (a short run) | 115.1, 88.6% |
| Rust, five minutes | not measured | 117.5, 92.7% |
| Afghan, five minutes | 102.2, 62.3% (a short run) | 117.4, 92.1% |

- The model lighting volume (512x256x4, tiled, 2 MB) has a few pages
  rewritten every frame, and all of it was signed, read, swapped and staged
  again at a segment's end of every frame: a sixth of the ring consumer. It
  is read and staged in the boxes of the pages written (`texture_cache.cpp`,
  `ReadInParts`; `gpu::watch::WrittenSince`; `gpu::TextureBoxesByPage`):
  37 KB a frame in place of 2 MB, "reading textures" 20% -> 4% of the
  consumer, the consumer 87% -> 65% of its core in the opening.
- A segment that nothing was uploaded for is joined to the one before
  (`renderer_frame.cpp`, `CloseSegment`): 47 rendering instances a frame
  over the scene's targets -> 15, the opening 87.7 -> 93.1 FPS and 11.1 ->
  10.4 ms. The picture is the same: at the flight's last view two
  screenshots with and without differ by 0.01-0.03/255 on average, two of
  one run by 0.05.
- An Adreno has the pixel constants as a uniform block with the subgroup
  ballot too (`pipeline.cpp`): the flight 8.7 -> 8.3 ms.
- `vkGetFenceStatus` is not called without a shadow of guest memory: nothing
  read it, and Turnip builds on Mesa 26.3 sleep in it (PurpleVK T30 62 -> 85
  FPS).
- What holds the opening at 93 is the GPU, 10.4 ms at 97% busy, and behind it
  the consumer: the title's main thread waits (`sub_8238EC58`) until the
  consumer has parsed the frame before's swap, so the consumer's 7-8 ms of
  processor time a frame there is on the frame's path as well. A hardware
  draw costs about 4 us of GPU (`MW2_NO_MERGE=1`: 38.8 ms a frame against
  11.4), so nothing that ends runs of draws pays.
- Drivers, in the opening: Turnip R2 85.1 FPS, v27.0.0-20261117 84.1, faux123
  1.2.10 84.8, mainline v32 63.8, PurpleVK T30 85 (the last two without
  `VK_KHR_present_wait`). PurpleVK reports 7.3 ms of GPU a frame, which its
  timestamps under-report: at `MW2_SCALE=2` it runs 39.7 FPS, as R2 does.
  `TU_DEBUG` sysmem, nolrz, nolrzfc, hiprio and `TU_AUTOTUNE_ALGO` profiled
  and bandwidth change nothing; gmem 18.0 ms, prefer_gmem 15.1.
- Heat: the hottest zone is at 92-96 C through the five-minute runs with the
  GPU at 680 MHz where it is busy.
- Rust started with `MW2_INPUT_SCRIPT=60:a,64:a` ends in the END GAME? dialog
  over the team menu; it plays without the script.
- Modern Warfare 3, Survival on Resistance in a `.dev` build, started from
  the menus (Special Ops, Solo Play, Survival, Resistance): 78.5 FPS in play
  with these changes, 74-81 every second, against the 72-74 of before, and
  the picture as it was. Started with `devmap so_survival_mp_paris` instead
  the picture is washed out to white (nine pixels in ten near white), and
  is on the `android` branch without any of these changes as well: that is
  the start, which skips what the menus set up, and not the renderer.

### Later that day: the game package, a vertex's loads, detail at distance

AYN Thor, as above. The Retroid Pocket Flip 2 ran flat during the work, so
nothing below is measured on the Adreno 650 except where said.

- **A game package held both games' libraries.** Setup collected them in
  one `out\game-libraries` and `android/game` packages the folder it is
  given, so once a PC had built both games each package had all four:
  `mw2-game.apk` 98.3 MB. A folder for each game now, and the Gradle module
  refuses a folder with anything else in it: 39.8 MB (`libmw2game.so`,
  `libmw2mpgame.so`) and 53.9 MB (`libmw3game.so`, `libmw3mpgame.so`). The
  Modern Warfare 2 package loads on both devices (campaign and multiplayer
  on the Thor, campaign on the Flip), Modern Warfare 3's on the Thor.
- **What a vertex loads** ([shaders.md](shaders.md#the-module-interface)): 66 loads
  from the arena a vertex over Team Player (counted on the Flip,
  `MW2_SHADER_STATS`), 30 of them words of the constant table. One load of
  the table for each block of constants, the window transform as a vec4,
  one dword a vertex fetch, a packed format's dword once, and the two-step
  endian exchange: 27 loads.

  | Thor, 120 Hz | before | after |
  |---|---|---|
  | S.S.D.D. opening, FPS / GPU ms a frame | 93.7 / 10.13 | 104.1 / 9.14 |
  | S.S.D.D. flight, FPS (frames a display period apart) | 108.5 | 112.2 (69.7%) |
  | Team Player, three fixed views of its opening, FPS | 65.0, 65.4, 72.3 | 71.4, 70.6, 76.7 |
  | ...the scene's GPU ms a frame | 12.50 | 11.47 |
  | Rust, five minutes, FPS | 117.5 (92.7%) | 118.5 (90.5%) |

  Step by step in the opening: the table 100.4 FPS (9.32 ms), the vec4, the
  fetch dword and the packed dwords 101.6 (9.24), the endian exchange 104.1
  (9.14). Six fixed views of S.S.D.D. differ from the build before by no
  more than two runs of one build do (0.2 to 1.9 of 255).
- **Detail at distance** ([rendering.md](rendering.md#detail-at-distance)):
  the title's LOD settings are registered and do nothing, so
  `XModelGetLodForDist` is hooked (`MW2_LOD_BIAS`, the app's Detail at
  distance). The opening of S.S.D.D. at 1, 2, 4, 8: 101.9, 111.2, 116.0,
  116.9 FPS; its flight at 2: 115.8 (79.5%). On the Flip, before it ran
  flat, 8 gave Team Player's heaviest views 0 to 3 FPS. The default is the
  console's on every device until S.S.D.D. is measured on the Flip.
- **The driver's pipeline cache file** was not loaded above 64 MB
  (`pipeline.cpp`), and it keeps every form of a shader it ever compiled: a
  cache of 60 MB went past that with these shaders' new forms, was refused
  at the next start and begun again, after which each run compiled what it
  reached (36 s of compiling in a 75 s run). Bounded since, below.
- Turnip aborts when a pipeline is built to capture its internal
  representations on Android (`fputc` of a null `FILE*`), so the driver's
  own listing of a shader is not to be had there; only its statistics.
- Modern Warfare 3, Survival on Resistance from the menus in a `.dev` build:
  the picture as before by eye, 85.9 FPS in play.
- Not measured: the Flip 2 after these changes (S.S.D.D. five minutes, Team
  Player's flight, Terminal).

### The pipeline cache in several files, and a multiplayer play-through

AYN Thor, Turnip v27.0.0-R2, `.dev` builds.

- **The limit was this runtime's.** Mesa's `vk_pipeline_cache_load` reads
  as many objects as a blob says; `pipeline.cpp` refused a file over 64 MB.
  The Thor's file was 47.2 MB: 5,656 objects of 8.7 KB for 1,571 recorded
  pipelines. Made 70 MB with copies of its own objects under other keys,
  the build before logs "size invalid (73431085 bytes), ignoring" and
  S.S.D.D. flown for 100 s compiles 633 pipelines in 53.8 s, the longest
  frame 2,862 ms.
- **Several files under a budget**
  ([rendering.md](rendering.md#the-drivers-cache-in-several-files),
  `MW2_PIPELINE_CACHE_MB`, 64 on Android). The same 70 MB file is loaded
  and closed whole: 1,571 of 1,572 pipelines found, 294 ms, the longest
  frame 56 ms. Being over the budget it goes at the next start, and its
  pipelines are compiled once more into files of 8 MB.
- **The Thor's own 47 MB file**, two runs of S.S.D.D.: 1 of 1,572 compiled
  (349 ms, longest frame 59 ms), then 2 of 1,574 (298 ms, 46 ms).
- **With a budget of 32 MB** the 47 MB file goes at the start. Runs of
  100 s then compile 531, 306, 263, 256 and 162 pipelines, the warm-up
  working through the recorded ones behind play (longest frame 3,017 ms in
  the first, 52 to 59 ms after), and the next compiles none: 1,574 found
  in 266 ms, in six files of 25.2 MB.
- **Modern Warfare 3, Survival on Resistance from the menus**, 210 s: with
  no cache 473 of 796 compiled in 23.5 s (longest frame 8,262 ms); the
  second run 3 of 512 (170 ms, 143 ms), the third 1 of 513 (109 ms,
  137 ms). The file is 7.4 MB.
- **Modern Warfare 3's multiplayer played through**: System Link, Seatown,
  Team Deathmatch, six bots ("Bots in my matches"), the vote on. Nebula of
  the camouflage pack chosen in Create a Class is on the MSR in the match.
  The match ran its ten minutes (`scr_war_timelimit` set from
  `MW2_CONSOLE` in the menus did not hold once Game Setup started the match:
  the clock began at ten); "Vote for the next map" came up under the result with three
  choices, LB took Lockdown, and it loaded with all seven clients and was
  played for three and a half minutes. No "Server command overflow" in
  the log.
- **Fixed on the way**: that vote offered Bakaara as One Flag CTF, one of
  the game types the disc left unfinished; they are no longer drawn
  (compiled, not run again).

### Vertex data in host byte order, and vertex attributes not

AYN Thor, Turnip v27.0.0-R2, `.dev` builds, performance mode, every
variant's pipelines compiled beforehand (`MW2_SHADER_PREWARM_ALL`), two runs
each way unless said. `MW2_HOST_ORDER=0` is the build before.

- **The copy exchanges the bytes, not the shader**
  ([rendering.md](rendering.md#vertex-data-in-host-byte-order)). The log
  says which at the start: "vertex data is copied in host byte order".
- **S.S.D.D., the opening** (seconds 26 to 42): 103.4 and 103.1 frames a
  second before, 105.9 and 105.9 after; 9.19 and 9.22 ms of GPU a frame to
  8.99 and 8.99; the scene's pass 7.25 and 7.27 ms to 7.05 and 7.05.
- **Team Player, three heavy views**: the scene's pass 11.81 and 11.25 ms
  to 10.93 and 11.00, the GPU 13.77 and 13.22 ms to 12.89 and 12.96, the
  run 73.5 and 75.4 frames a second to 76.4 and 76.5. Vertex data copied a
  frame is 9 MB and drawn from kept copies 54 MB either way.
- **S.S.D.D. flown** (one run each): 112.8 to 113.3 frames a second, the
  scene 4.15 to 3.96 ms. **Rust** with bots, 150 s: 117.9 to 118.1, the
  scene 3.79 to 3.64 ms.
- **Modern Warfare 3, Survival on Resistance from the menus**: 86.4 and
  88.1 frames a second in play before, 92.2 and 91.1 after.
- **The picture.** S.S.D.D. at six fixed views, the mean difference of a
  pixel's channels out of 255: 0.06 to 1.41 between the two at five of
  them, where two runs of one differ by 0.03 to 1.78; 5.1 at the sixth,
  where soldiers move, and 2.2 to 6.1 between runs of one. Survival's
  first view: 4.9 between the two, 3.0 and 5.1 between runs of one.
- **Retroid Pocket Flip 2** (Adreno 650, Turnip R2, each way's pipelines
  compiled beforehand, the two alternated). Team Player's heavy views:
  42.9 and 42.5 frames a second before, 44.9 and 46.2 after; the scene's
  pass 16.95 and 17.1 ms to 15.99 and 15.65; the GPU 22.1 and 22.2 ms to
  21.1 and 20.8. S.S.D.D.'s opening at the limit of 60: 59.3 before and
  59.4 after, the scene 10.70 ms both ways. The experiment build before
  it, one run each way on a low battery, had shown none (the scene 16.49
  and 16.63 ms).
- **Vertex attributes lose.** A vertex shader's first stream as a vertex
  buffer, fetched by the fixed-function stage, the rest as before (branch
  `vertex-input`, not merged). Merged draws need their vertices in one
  buffer, so in Team Player 2,290 draws a frame had theirs under another
  draw's record and 2,210 were copied again: 40 MB of vertex data a frame
  against 9. In S.S.D.D.'s opening a hardware draw was 8.5 of the title's
  against 11, and the ring consumer at 98% of its core against 81%. Thor:
  that opening 103.6 to 82.3 frames a second (GPU 9.18 to 9.63 ms), Team
  Player's views
  74 to 49; for the world's shaders only (9 to 13% of the fetches) 103.8
  and 62. Flip 2: the scene 16.49 to 17.32 ms, 18.05 and 19.39 for the
  world's shaders only, and twice to four times the draws skipped for want
  of arena.

## Build (WSL2 Ubuntu 24.04)

Game files go in the git-ignored `mw3/tu0/` (`tools/xdvdfs.py <iso> mw3/tu0`).

```sh
export MW2_TITLE=mw3 MW2_VERSION=tu0
bash tools/recomp_mw3.sh                 # XEX -> ppc_mw3/ (never committed)
python3 tools/genshared.py && python3 tools/gen_kernel_stubs.py
unset MW2_TITLE MW2_VERSION
bash android/fetch_deps.sh               # optional: libadrenotools
cd android && echo "sdk.dir=$ANDROID_HOME" > local.properties
MW2_LTO=OFF ./gradlew assembleMw3Tu0Debug
```

Needs JDK 17, NDK 28.2.13676358, `platforms;android-35`, CMake 3.22.1+,
clang-18, Ninja, Python 3. The APK contains code recompiled from the game
executable: it is for the owner's device only and must not be published.
