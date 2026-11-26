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
| PC build | the mouse turns the view by an angle (110.0 degrees a thousand counts at sensitivity 5, measured with a scripted mouse) and points in Modern Warfare 3's menus; a System Link match on Seatown with six bots played to its end, the vote and the next map by script, 53 frames a second in play beside another game on the same GPU. Not yet tried with a hand or with real Windows input: the cursor's hiding and confinement, Alt+Tab, fullscreen, the wheel |
| Settings in the game | verified on the Thor and the Flip 2 in all four titles: the app's menu over the game (touch, or Select and Start on a controller) changes resolution, sharpening, FPS limit, sun shadows, field of view, detail and texture memory under the running level; the on-screen controls stay off across restarts ([below](#the-menu-over-the-game-24-november-2026)) |
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

### The Flip 2 at 60: the driver's switches, two more drivers, compression

Retroid Pocket Flip 2 (Adreno 650), performance mode, `.dev` builds, the
bundled Turnip v27.0.0-R2 unless said. The numbers and the reasons are in
[android.md](android.md#modern-warfare-2-there-60-frames-a-second).

- **Nothing of the driver's moves Team Player.** At its three heavy views
  (41 to 46 frames a second from one run to the next, the scene 15.4 to
  18.0 ms): `TU_AUTOTUNE_ALGO=prefer_sysmem` 43.9, `profiled` 41.6,
  `TU_AUTOTUNE_FLAGS=tune_small` 44.5, `TU_DEBUG=sysmem` 42.9, `nobin`
  41.9, `nolrz` 42.0, `IR3_SHADER_DEBUG=noearlypreamble` 42.4; `gmem` 32.7.
- **Drivers.** TurnipZ 0.4.0 46.3 and 44.4, the same as R2; K11MCH1's
  Turnip 24.1.0 R18 for a6xx 34.2, 32.1 and 32.2. R2 stays bundled.
- **LRZ.** Turnip gives it up at the title's pair of clear rectangles
  (depth written with the test ALWAYS). With those made a clear of the
  depth image the message goes and the scene's time does not change; the
  title's stencil tests and blended depth writes end LRZ's writes anyway.
  Not kept.
- **Barriers**: 32 a frame, none without a change of layout.
- **Image compression** left on, whole and one image class at a time
  (through `VK_EXT_image_compression_control` in a test build): 41.6 to
  45.3 frames a second against 45.9 without, and no reset in six starts of
  Team Player. `noubwc` stays.
- **The clock**: 670 MHz, the device's most, in seven readings of eight
  under load.
- **The scene's size** tells what is left: 512x300 gives 52.5 frames a
  second there, so some 5 ms of the scene are its pixels and 11 its
  vertices.
- **As merged, with vertex data in host byte order**: S.S.D.D. flown for
  five minutes 59.8 frames a second, 92.0% of the frames on time (59.9 and
  91.5% before any of this); Team Player's flight 52.6 and 56% (52.5 and
  57%); Terminal 59.5 and 94.8% (59.3 and 95.5%); Rust 59.4 and 95.6%
  (59.3 and 95.7%). Team Player's first half is still 37 to 50. The GPU's
  zone reached 62 C, the processor's 71, the skin's 51.
- **Modern Warfare 3** in a `.dev` build, at its limit of 30 on this
  class: Survival on Resistance from the menus 30.0 frames a second in
  play, Black Tuesday from Mission Select 29.7, the picture as before by
  eye. Started with `devmap sp_ny_manhattan` instead, the campaign stops
  at "Unable to write to default save device" over a black screen, on
  this build and on the one before the day's changes alike; why was not
  looked into.
- The device charges at 5 V and 0.5 A and lost a percent of battery every
  three minutes of play; the runs were spread over the charge it had.

## 2026-11-21

### What a vertex shader costs on the Flip 2, and the arena as texels

Retroid Pocket Flip 2 (Adreno 650), performance mode, `.dev` builds, the
bundled Turnip v27.0.0-R2. The reasons are in
[shaders.md](shaders.md#the-module-interface), the numbers in
[android.md](android.md#modern-warfare-2-there-60-frames-a-second).

- **Asking the driver.** `MW2_SHADER_IR=<dir>` (with `MW2_SHADER_STATS=1`)
  writes what Turnip compiled each vertex shader to: the NIR and the ir3
  assembly, binning variant apart. `IR3_SHADER_DEBUG=disasm` and this both
  aborted the game at the first pipeline ("FORTIFY: fputc: null FILE*"):
  Turnip's disassembler opens `/dev/null`, the driver's file redirect sends
  that to `driver_files/dev/null`, and there was no such folder. There is
  now.
- **What it said.** Two vertex shaders run three vertices in five of Team
  Player. The depth pass's (37% of them) was 207 instructions, 45 of them
  fetches, in 21 registers, six waves a core; the lit world's (25%) 348, 61
  and 32, four waves. On a 6xx ir3 reads a storage buffer one dword a fetch
  (`isam` of a buffer texture, an add for the address, a move of zero for
  the second coordinate), so a vec4 was twelve instructions, and the loads
  waited on loads four deep. The position alone is most of it: the depth
  pass's shader is its own binning variant, the lit world's is 229
  instructions. The driver already drops the outputs a pixel shader does
  not read (9 of 16 left in the lit world's), so there was nothing for the
  translator to drop.
- **Where the scene's time is.** With every primitive put outside the clip
  volume (a test build, not kept; the GPU's clock at 587 MHz, having less
  to do): 3.9 ms a frame with vertex shaders that compute nothing, 10.3 with
  the real ones once their vec4 were texels (the first step; before it was
  not measured this way, and the uncut scene was half a millisecond more).
  So of the scene's 15 to 16 ms some 6 to 7 were vertex shading, 3.5 to 4
  its 550,000 primitives however they are shaded, and 6 pixels.
- **The arena as texels** (`MW2_VERTEX_TEXELS`, the default on an Adreno
  6xx under Turnip). Set 2 has the slot's share twice more, as texel buffer
  views of four floats and of four dwords. A vertex shader fetches a vec4
  as one texel; its draw table entry is two texels, eight dwords, whose
  last six hold what it loaded through the record before (the first dword
  of its fetch constants, the places of its blocks of constants); and
  vertex data the renderer copied is fetched four dwords a texel, the copy
  starting at a multiple of four. The depth pass's shader: 99 instructions,
  11 fetches, 10 registers, twelve waves. The lit world's: 204, 16, 16,
  eight. The culled scene: 7.5 ms, so 3.5 of vertex shading for 6.4. What a
  vertex computes is untouched.
- **Measured**, against `MW2_VERTEX_TEXELS=0` in the same build, each with
  its pipelines cached: Team Player's three heavy views 43.8, 44.2 and 45.9
  frames a second without, 50.0, 51.5, 52.6 and 53.5 with (the scene 15.7
  to 16.8 ms; 12.5 to 14.1); its flight 53.0 and 52.9 with 54.5 and 55.8% of
  the frames on time, 56.6 and 56.2 with 69.3 and 69.4% (the first half
  45 to 51; the worst ten seconds 39 and 41 to 47 and 49; 16.1 ms of GPU a
  frame to 14.6 and 14.8). With it, S.S.D.D. flown for five minutes 59.9
  and 93.7% (12.4 ms), Terminal 59.8 and 96.0%, Rust 59.5 and 95.8%.
- **The picture.** Four fixed views of S.S.D.D., two runs each way: 64.3 to
  99.7% of the pixels below the status line are the same to the bit with
  and without, 65.3 to 99.7% between the two runs without; in the stillest
  view 0.32% differ either way, the helicopters.
- **Not done.** Team Player is not a locked 60: its first half is 50. The
  window transform, a fetch and seven instructions a vertex that are the
  identity for every draw but the clears, could be a second form of the
  shader. No other GPU has run the texel form; the Thor was not to hand.
- **Modern Warfare 3 with it**, later that day, against
  `MW2_VERTEX_TEXELS=0` in the same `.dev` build, each with its pipelines
  cached, the levels started from the menus (Mission Select opened with
  `mis_cheat 1` on a new profile, whose New Game is the Prologue). At the
  limit of 30, without and with: Survival on Resistance 30.0 and 30.0
  frames a second, 90.9 and 89.9% of the frames within two display periods,
  25.3 and 23.8 ms of GPU a frame; Black Tuesday's opening 29.6 and 29.8,
  89.8 and 91.1%, 25.1 and 23.1 ms; Seatown with six bots 30.0 and 30.0,
  89.5 and 88.3%, 20.0 and 18.3 ms from the same spawn. With the limit at
  60: Survival's first spawn 42.3 and 46.4 frames a second, 23.5 and
  21.6 ms (the scene 16.6 and 15.3); Black Tuesday's opening 42.1 and 45.3,
  22.4 and 20.7 ms (17.7 and 16.1). No renderer error either way. The
  picture: the campaign's menus 99 to 100% the same to the bit with and
  without at 30 (53 to 100% over all the menus' pairs, 56 to 86% between
  two runs of one way); at the first spawns the mean difference of a
  pixel's channels 1.2 to 5.1 of 255 with and without, 3.3 or more between
  two runs of one way. The default stays for both games.
- The device charged at 6 to 10% an hour; the runs were made between 20
  and 28%.

### Team Player at 60 on the Flip 2: the pixel shaders' record as texels

The same device and driver, `.dev` builds. The reasons are in
[shaders.md](shaders.md#the-module-interface), the tables in
[android.md](android.md#modern-warfare-2-there-60-frames-a-second).

- **Tried, in the order of what it gave.** The pixel shaders' fetches
  (kept, below). The vertex shader without the window transform: the scene
  11.2 ms against 11.3, nothing one run does not differ from the next by;
  not kept. Primitives: counted (`MW2_COUNT_PRIMITIVES`), 860,000 triangles
  in 8,200 draws a frame, 91% with back faces culled as the title's
  `PA_SU_SC_MODE_CNTL` says, draws of one or two triangles 537 a frame with
  927 triangles between them; nothing to take away.
- **`MW2_PIXEL_TEXELS`**, the default on an Adreno 6xx under Turnip. A
  pixel fetched 26 to 44 single dwords of the storage buffer in a lit
  surface's shader, asked each texture its size and took two reciprocals
  and an `exp2` a texture. The record now holds a texel a texture with the
  slot's word, the LOD bias, the exponent adjust and one over the size.
  Team Player's heavy views 48 to 51 frames a second without, 59 to 60
  with; the frame 18.3 to 19.1 ms of GPU, 15.2 to 15.4; its flight 59.5
  frames a second with 87.9% on time and a first half of 59.0, for 56.6,
  69.3% and 51. S.S.D.D. five minutes 59.9 and 93.4%, Terminal 60.0 and
  97.5%, Rust 59.9 and 95.6%.
- **The picture.** Four fixed views of S.S.D.D. against the captures
  without from earlier in the day: 66.9 to 92.6% of the pixels the same to
  the bit, 65.6 to 94.6% between two runs without, and the differences in
  the same places.
- **Modern Warfare 3 with it**, the same day, against
  `MW2_PIXEL_TEXELS=0` in one `.dev` build with the cache warm for each:
  Survival on Resistance at the limit of 30, 30.0 frames a second both
  ways, 24.2 ms of GPU a frame without and 18.2 with; with the limit at 60,
  46.1 frames a second and 58.2 (21.6 ms; 16.6). Black Tuesday's opening
  29.8 and 30.0 (22.9 ms; 15.9), and 46.3 and 55.8 at 60 (20.1 ms; 15.0).
  No renderer error; the picture differs with and without by what two runs
  of one way differ by. The `.dev` app and the copy of its files are off
  the device again.
- The device stopped charging at 22% (a restart set it going) and was off
  `adb` for a while; the Modern Warfare 3 runs were made once it was back.

### No draw skipped for want of arena, and the texels on the Adreno 740

AYN Thor (Adreno 740, 120 Hz) and Retroid Pocket Flip 2, the bundled Turnip,
`.dev` builds. The reasons are in
[rendering.md](rendering.md#a-frame-larger-than-its-slots-share), the table
in [android.md](android.md#adreno-7xx-snapdragon-8-gen-2-class).

- **What was skipped.** Team Player's flight logged 34,197 draws skipped as
  `frame arena full` on the Thor and 8,486 on the Flip 2. A frame whose slot
  keeps no vertex data yet copies 105 to 116 MB of windows, of a 128 MB
  share: the level's first frames, the frames after each checkpoint loaded
  again, and three frames, one a slot, each time a slot let its copies go,
  which was whenever its frame's own ran out of room (13 to 17 times a
  flight). The kept ranges lay over each other and lost their places: 76 MB
  of a share for 41 MB of vertex data.
- **What changed.** A draw with no room is made in the next slot after a
  submission; a slot's copies stay when its frame's own run out; vertex data
  is kept by blocks that lie apart, their places used again, written pages
  copied again one by one, and the ranges not drawn from lately let go
  first.
- **After.** No draw skipped as `frame arena full` on either device in Team
  Player's flight (1 or 2 draws made in the next slot, no slot letting its
  copies go), five minutes of S.S.D.D. or Modern Warfare 3's Survival. The
  flight's frame rate is as it was: 95 to 97 frames a second on the Thor,
  59.6 to 59.7 with 88% on time on the Flip 2. Screen recordings of the
  flight before and after show the frames after a checkpoint with the scene
  missing, and whole.
- **The texels on the Thor.** `MW2_VERTEX_TEXELS=1` with
  `MW2_PIXEL_TEXELS=1` against both off, GPU ms a frame: S.S.D.D.'s opening
  9.08 to 7.43, Team Player's heavy views 13.16 to 10.35, Rust 6.48 to 5.54,
  Modern Warfare 3's Survival 10.87 to 8.52 (86.7 frames a second to 107.8).
  The vertex form alone is two thirds of it. The picture is the same, so
  both are the default on every Adreno under Turnip.
- The Modern Warfare 3 `.dev` app and the copy of its files are off both
  devices again.

## Stutter on the AYN Thor (22 November 2026)

Measured with `MW2_REPORT=1` on `.dev` builds, four to four and a half minutes of play
each, Turnip v27.0.0-R2 at 120 Hz. Stutters are world frames half a blank late or more;
"long" is the longest frame.

| Run | Before: stutters / long / fps | After: stutters / long / fps |
|---|---|---|
| MW3 Seatown, empty caches | 550 / 463 ms / 107.4 | 91 / 68 ms / 118.6 |
| MW3 Seatown, second session | 436 / 1283 ms / 105.8 | 54 / 81 ms / 118.9 |
| MW3 Bakaara, first visit / second | not measured | 123 / 67 ms / 118.5; 387 / 50 ms / 115.9 |
| MW3 Survival, empty caches / second | not measured | 272 / 79 ms / 117.5; 126 / 126 ms / 118.2 |
| MW2 Afghan, empty caches | 286 / 814 ms / 110.7 | 86 / 83 ms / 118.6 |
| MW2 Afghan, second session | 91 / 264 ms / 117.3 | 34 / 71 ms / 118.8 |
| MW2 S.S.D.D., second session (same inputs) | 1208 / 57 ms / 96.4 | 631 / 72 ms / 97.4 |

What it was, in the order of what it cost:

- The relay's keep-alive thread never slept (`online/server.cpp`: a quarter of one second
  in whole seconds is a wait of none). It ran a whole core in every multiplayer session,
  usually the fastest one, which the ring consumer was pinned to and could not leave: the
  consumer, runnable, got 0.19 s of processor in 4.4 s. The thread sleeps now, and the
  consumer may run on the performance cores too.
- Pipelines compiled on the renderer's thread, one a frame (rendering.md).
- Modern Warfare 3's model lighting volume read whole every frame, and BC textures
  decoded in software under Turnip (textures.md).
- The warm-up and the cache writes held locks the consumer takes, at lowered priority.

S.S.D.D.'s opening is bound by the ring consumer (96% of a core at 97 frames a second),
not by any of the above. Pipeline libraries were tried again under Turnip
(`MW2_FORCE_PIPELINE_LIBRARIES=1`): four minutes without a fault, but 3,416 compiles and
54 MB of cache for one map, 2.1 GB of memory against 0.8, and links waiting up to 184 ms
behind the optimiser; not made the default.

## The ring consumer profiled on the AYN Thor (24 November 2026)

Modern Warfare 2, `.dev` builds, Turnip v27.0.0-R2 at 120 Hz, performance
mode, `MW2_REPORT=1`, pipelines cached. The scenes: S.S.D.D.'s opening
(seconds 26 to 42 of `devmap trainer`), Team Player's three heavy views
(`setviewpos`, ten seconds each), and a match on Rust with six bots
(`map mp_rust`, `MW2_BOTS=6`).

- **Where the build before stands.** S.S.D.D.'s opening 118.5 to 119.1
  frames a second with 86 to 89% of the frames on time (the consumer 82 to
  95% of its core, the GPU 7.2 ms a frame); Rust with bots 119.0 and 89 to
  90% (the consumer 61 to 65%); Team Player's views 82.2 to 83.9 (the
  consumer 96 to 97%, the GPU 9.9 ms). The 97 to 106 the opening had were
  not met again: the device ran cooler than in the runs they come from
  (the processor's zone 77 to 88 C here, 92 to 96 C then).
- **The consumer's time, by simpleperf** (cpu-clock, 4 kHz, twelve to
  fourteen seconds a scene; S.S.D.D. / Team Player / Rust, % of the
  thread's samples):

  | | S.S.D.D. | Team Player | Rust |
  |---|---|---|---|
  | vertex data copied (`CopyVertices`) | 18.3 | 12.9 | 7.6 |
  | a draw's indices read, widened and stored (`WriteIndices`) | 13.3 | 14.2 | 11.1 |
  | PM4 parsing and register writes | 9.6 | 11.1 | about 9 |
  | texture look-ups and checks (`TexturesFor` and below) | about 9 | 11.5 | about 9 |
  | kept vertex ranges looked up (`KeptWindow`) | 6.3 | 8.0 | 8.7 |
  | the rest of a draw's state (`DrawInSlot` itself) | 5.1 | 5.6 | 6.0 |
  | float constants | 5.0 | 5.6 | 5.3 |
  | shader and pipeline look-ups, shader loads | about 4 | about 5 | about 6 |
  | the memory watch's faults and protection changes | about 1 | about 2 | 1.5 |

  A draw record's descriptors do not appear: sets 1 and 2 are bound once a
  command buffer, and the Vulkan calls are the recorder thread's (24 to
  29% of a core).
- **What the time is.** Waiting for memory, not arithmetic. The indices'
  cost is the read of their first lines, in memory the processor has not
  been to this frame: with the widening and the store moved to another
  thread the read alone cost what all three had. The copy of vertex data
  runs at 6 GB a second into the arena's write-combined mapping. The PM4
  stream is cold as well: reading a dozen packet headers ahead cost eight
  times what a draw's own packet does.
- **Kept**: the look-ups (changelog; [rendering.md](rendering.md#vertex-data-kept-without-the-shadow)).
  `KeptWindow` 6.3% of the consumer to 3.9% in the opening, the shader
  loads' map 1.1% to under 0.5%, the pipelines' map 1.0% to 0.6%. Frame
  rates before and after, the two builds in turn: the opening 118.8 and
  118.5, then 118.8, 118.6 and 118.9; Team Player 82.6 and 82.2, then 84.0,
  83.0, 85.7, 83.1 and 81.5; Rust 119.0 both. No more than two runs of one
  build differ by.
- **Tried and not kept**, Team Player's views unless said:
  - *The arena in memory the processor caches* (Turnip's cached, coherent
    memory type): 70.7 frames a second for 82.6, the GPU 10.7 ms for 9.9.
  - *A copy thread for every copy and the indices* (8,000 jobs a frame,
    made before the title is told its draws are done and before the frame
    is submitted): 71.6 while the consumer waited for a thread that slept;
    83.0 once the consumer made what was left at a join itself; 85.0 with
    the indices' ranges remembered while the watch says they are unwritten.
    The thread took 75% of a core, most of it looking for work.
  - *A copy thread for copies of 4 KB and more* (500 jobs a frame, nine
    tenths of the bytes): 85.4 and 85.3 with it, 85.7 and 83.1 without in
    the same build; in the opening the consumer 85 to 86% of its core for
    92 to 93%, the frame rate and the share on time unchanged (118.8 and
    87.2%; 119.0 and 88.2%), for 63 to 73% of a core. With the scene at
    512x300, where the GPU is not near its limit, 89.6 with it and 86.7
    without.
  - *The next draw's indices asked of memory a draw ahead* (prefetch):
    `WriteIndices` 13.3% to 12.3%, and finding the next draw's packet 7.5%.
- **What holds Team Player at 83.** The consumer, on its core all the
  time, and behind it the GPU at 9.9 ms: with a quarter of the scene's
  pixels the views run 86.7. The title's main thread waits 45% of the time
  for the swap before to be parsed, and 11% of it for the kernel
  dispatcher's one lock in `NtSetEvent` (`kernel::Event::Set`), which
  wakes its waiters while it holds it; not changed here.
- **Not possible here**: huge pages for guest memory (the kernel has
  transparent huge pages off).
- **The picture.** Four fixed views of S.S.D.D., pixels the same to the
  bit below the status line: 99.0, 99.7, 69.1 and 94.1% between the build
  before and this one, 98.9, 99.7, 67.0 and 96.8% between two runs of the
  build before.
- **Other titles with it**, `.dev` builds on the Thor: Modern Warfare 3's
  Survival on Resistance from the menus, first visit, 117.3 frames a second
  and 84.1% on time (117.5 before, in the stutter runs above); Call of
  Duty 4's Charlie Don't Surf 116.4 and 85.6% on its first visit, 118.2 and
  86.0% on its second (117.7 and 87.2%; 117.4 and 86.5%). On the Retroid
  Pocket Flip 2, Modern Warfare 2's S.S.D.D. opening at its limit of 60:
  59.9 frames a second, 94.2% on time, the consumer 52% of its core.

## Sharpening, upscalers and frame generation (24 November 2026)

AYN Thor and Retroid Pocket Flip 2, Turnip v27.0.0-R2, `.ngg` builds, Modern
Warfare 2. The tables and the reasons are in
[rendering.md](rendering.md#sharpening-on-the-way-to-the-window).

- **Kept: sharpening** (`MW2_SHARPEN`, the app's Sharpening setting, off by
  default): FSR 1's RCAS in place of the window's linear copy, at the
  screen's full size. 0.54 ms of GPU a frame on the Flip 2 against 0.19 for
  the app's default path, 0.66 against 0.18 on the Thor.
- **Seen on the way:** the app's default path -- a surface of 70% of the
  screen, enlarged by the compositor -- is 0.5 to 0.8 dB further from a
  picture drawn at twice the size than a linear copy into a full-size
  surface, which costs 0.13 to 0.17 ms a frame more. The default was left.
- **Not kept:** Snapdragon GSR 1 (RCAS's cost, a lower SSIM, a harsher
  picture), FSR 1's EASU (twice the cost and more for RCAS's PSNR), frame
  generation from two frames (60 to 120 on the Thor: 1.8 ms of GPU a frame,
  a frame on screen 18.5 ms later, 31 dB against the real frame where
  holding the last one is 24, and the HUD smeared over a moving world; 30
  to 60 on the Flip 2: 3.4 ms, 17 ms, 26 dB).
- **Seen on the way:** at a limit of 30 on the Flip 2 a frame is presented
  115 ms after the renderer finished it: the renderer is three frames ahead
  of the window there. At 60 on the Thor it is 2 ms. Not looked into.
- **Not built:** the temporal upscalers and TAA (no motion vectors, no
  jitter, the HUD in the picture), variable-rate shading (Adreno 7xx only
  under Turnip, where pixels are not the limit), Qualcomm's frame
  extrapolation (OpenGL ES on the phone's own driver; listed by the Thor's,
  not the Flip 2's).

## The menu over the game (24 November 2026)

`.gm` builds of the four titles on the AYN Thor (Adreno 740, 120 Hz) and the
Retroid Pocket Flip 2 (Adreno 650, 60 Hz), the game code packaged from
libraries built before this change (the game library's statement is
unchanged: they load). Each campaign started by `devmap` (S.S.D.D., Black
Tuesday, F.N.G., Seoul at `seoul_enemy_encounter_01`), the menu opened with
Select and Start on the device's own controller (`sendevent` on its input
device) and every row changed by its D-pad; Modern Warfare 3's multiplayer
in a split-screen match on Seatown, there by touch. What is in the design
is in [android.md](android.md#the-menu-over-the-game-gamemenukt). Pictures
and logs: `C:\Users\solim\mw3-android-work\out\gamemenu\`.

| Row | Modern Warfare 2 | Modern Warfare 3 | Call of Duty 4 | Advanced Warfare |
|---|---|---|---|---|
| Display resolution | live: the swapchain goes from 1344x756 to 672x378 and back, the level runs on (both devices) | live (Thor) | live (Thor) | live (Thor) |
| Sharpening | live: `sharpen:` lines 10 to 80% and off, the surface 1920x1080 while on (Thor) | live (Thor; on the Flip 2 seen going to 20%) | live (Thor) | live (Thor) |
| FPS limit | live: the frame-rate line reads 60, 45, 30, 45, 60, 118 on the Thor, whose display follows to 60 Hz and back, and 46, 30, 45, 60 on the Flip 2 | live: 60, 46, 31, 45, 60, 118 (Thor) | live: 120, 60 and 30 (61, 31, 60, 118), and 45 since 25 November (45 on the Flip 2's frame-rate line, set in the menu and at the start: the runtime holds the title's presents, [rendering.md](rendering.md#more-than-60-frames-a-second)) | live: 60, 45, 30, 45, 60, 118 (Thor) |
| Sun shadows | live: gone and back in the picture (both devices) | live by the log (`sm_enable` 0 and 1; the street is overcast and the pictures do not show it); Auto follows the limit, on at 60 and off at 120 | live: the window's light on the floor goes and comes back (Thor) | live by the log; not seen in the picture, the start point faces the sky |
| Field of view | live, `cg_fov` 65 to 110 (both devices) | live: `cg_fov` in the campaign, `MW2_FOV` in the multiplayer (Thor) | live, `cg_fov` (Thor) | live, `cg_fov` (Thor) |
| Detail at distance | live: `detail:` lines at 2 and 4 (Thor) | no hook | no hook | no hook |
| Texture memory | live: `perf: the texture budget is now 128 MB` (Thor) | the same code, not exercised | not exercised | not exercised |
| Frames per second shown | live (Thor) | the app's own view, as in Modern Warfare 2 | the same | the same |
| Internal render multiplier, Smooth edges | saved, marked `*`; `MW2_SCALE=2` and multisampling on at the next start (Thor) | the same code | the same code | the same code |

- **Kept across a restart** (Modern Warfare 2, Thor): every value set in the
  game was in the preference file after Quit, the launcher's Settings, in
  the process that had been running since before the game, showed them
  (35%, 2x, multisampling on, Low, shadows On), and after `am force-stop`
  and a new start the run began with them (`surface 672x378`,
  `MW2_SCALE=2`, `MW2_LOD_BIAS=4`, `MW2_SHADOWS=1`,
  `MW2_TEXTURE_BUDGET_MB=128`, `cg_fov 65`).
- **The on-screen controls off, and back on with the controller**: turned
  off in the menu by the D-pad, the game quit from the menu, a switch
  changed in the launcher (which used to write its stale copy back),
  `touchControls` still false; the app stopped and started: no controls
  drawn, the Menu button there; Select and Start, D-pad, A: controls back
  (`thor-mw2-restart-touch-off.png`, `thor-mw2-touch-back-on-by-pad.png`,
  `flip-mw2-touch-persist.png`). The same on the Flip 2. Physical
  controllers switched off in the menu by the controller were switched on
  again by it: the menu's own keys do not go through that switch.
- **By touch** (Modern Warfare 3's multiplayer, Thor): the Menu button, the
  Graphics row, the field of view's arrows, a touch beside the panel to
  close it.
- **The Flip 2, the other three titles** (the same script, after the
  table's Thor runs): Modern Warfare 3 reads 30, 45, 60 at those limits,
  Call of Duty 4 61 at 45 as on the Thor and 31, 60 at the others, Advanced
  Warfare 46, 30, 45, 60; the resolution, sharpening (the surface 1920x1080
  while on), `sm_enable` and `cg_fov` changed in each as on the Thor
  (`flip-<title>-sheet.png`).
- **Not done**: the multiplayer of Modern Warfare 2, Call of Duty 4 and
  Advanced Warfare (the same activity and runtime calls; not run). No
  frame rate here is a measurement: the numbers are the app's own
  frame-rate line, read off screenshots.

## Every mode of every game (25 and 26 November 2026)

One round over the four games' modes at `58b0f6b`: first on Windows (RTX
5080, the nine executables built from that commit, a scripted pad and a
second one for split screen: `MW2_INPUT_LIVE`, `a@2`), then on the AYN Thor
(120 Hz) and the Retroid Pocket Flip 2 (60 Hz) with debug `.am` apps of the
same commit, the game code of toolchain 2, each game read from the disc
image already on the device. A device run is 170 to 230 s with the pad
walking, turning and firing from the moment the level is up; the frame rate
is the log's presents per second over that time. The devices were shared
by lock with six other rounds, so the device half is not complete: what was
not run is said, and nothing below is carried over from an earlier day
unless it says so.

| Game | Mode | Windows | Thor | Flip 2 |
|---|---|---|---|---|
| Modern Warfare 2 | Campaign | New Game from the menu: the intro and S.S.D.D.'s load | S.S.D.D. (`devmap`) 115.4 FPS | S.S.D.D. 60.0 |
| | Special Ops, alone | The Pit from the menus, played | not run | not run this round (played there on 20 November) |
| | Special Ops, split screen | The Pit with two pads, both views in play | not run | not run |
| | Multiplayer with bots | not run | Rust with 6 bots by `map`: 115.6, the bots joined; the player was left at the class menu | the same, 60.0 |
| | Multiplayer, split screen | Team Deathmatch on Afghan, two pads, both in play | not run | not run |
| | Find Match through the relay | **failed**, two copies on one PC, twice: one lobby, the match starts, the copy that does not host ends in `EXE_ERR_SERVER_TIMEOUT` | not run this round (passed on 23 November) | the same |
| Modern Warfare 3 | Campaign | not run | Black Tuesday 118.9 | 60, by touch on the release-signed app |
| | Survival | Resistance from the menus, wave 1 | not reached (the run's presses went to the campaign's menu) | Resistance, 60, release-signed, by touch |
| | Special Ops mission | not run | not run | Stay Sharp, 60, release-signed, by touch |
| | Special Ops, split screen | Survival on Resistance with two pads; the load took 74 s against about 15 alone | not run | not run |
| | Multiplayer, System Link with bots | host with 4 bots on Seatown, a second copy listed it and joined | Dome with 6 bots by `map`: 119.1 | **from the menus**: Create Game, Seatown, 6 bots, 100 s in play, 60.0 (lowest second 58) |
| | Multiplayer, split screen | Team Deathmatch on Seatown, two pads, both in play | not run | not run |
| | Find Match through the relay | two copies, Team Deathmatch on Outpost, opposite teams | not run this round (23 November) | the same |
| Call of Duty 4 | Campaign | not run | F.N.G. 119.1 | F.N.G. 60.0 |
| | Multiplayer, System Link with bots | Create Game from the menus, Backlot, 4 bots, played | not run from the menus | not run from the menus |
| | Multiplayer, split screen | Team Deathmatch on Backlot, two pads, both in play | not run | not run |
| | Find Match through the relay | two copies, Team Deathmatch on Downpour, both scoreboards list both | not run this round (23 November) | the same |
| Advanced Warfare | Campaign | New Game from the menu, Induction's drop pod | Seoul 119.0 | Seoul 60.0 |
| | Exo Survival | not run: the campaign's entry asks for the other executable, which the app starts and a development run does not | not run this round (24 November) | the same |
| | Multiplayer | System Link, Create Game, Retreat, the loadout menu | not run | not run |
| | Multiplayer, split screen (Local Play) | Team Deathmatch on Retreat, two pads, both in play | not run | not run |
| | Exo Zombies, the owner's packages | Outbreak from Local Play, played (title update 17's executable, `MW2_LAUNCH_DATA`) | not run this round (25 November) | the same |

What the round found:

- **Split screen works on Windows in every title that has it**: the
  multiplayer of all four games and the Special Ops of Modern Warfare 2 and
  3, the second player joining on the titles' own sign-in screens. It has
  not been run on a device with two controllers. In Advanced Warfare the
  guest's pad, not the first player's, answers "Progress won't be saved".
- **Modern Warfare 2's Find Match between two copies on one PC fails**
  where Modern Warfare 3's and Call of Duty 4's pass in the same minutes:
  the lobbies merge, the host loads the map and plays, the other copy
  waits and ends in `EXE_ERR_SERVER_TIMEOUT` (logs
  `private-logs\allmodes\mw2_findmatch_*.log`). Whether two devices still
  meet as on 23 November was not run; until it is, that row is open.
- **Call of Duty 4's multiplayer started by `map` from the console** with
  the relay as its service ends in `EXE_LOSTCONNECTION` on both devices as
  the bots join; from the menus (System Link, Create Game) the same match
  plays on Windows. A shortcut of the test, not a player's path.
- **Modern Warfare 3's Special Ops has Find Match and Private Match
  entries that answer "You must be signed in to Xbox LIVE"**: the campaign
  app carries no online service. Solo Play and Split Screen are what work.
- **The first run's Brightness and margin screens take A only once the
  slider was moved** (the "Press A to continue" line appears then), in
  Modern Warfare 2, 3 and Advanced Warfare alike; and Modern Warfare 3's
  main menu takes no press for a few seconds after it appears. Both look
  like the titles' own; a player tapping A alone sees nothing happen.
- **A scripted run's exit** (`MW2_RUN_SECONDS`) of Modern Warfare 2's
  multiplayer on the Thor logs a guest fault after its report: a guest
  thread still running as the memory goes. Diagnostic builds only.
- On Windows, starting four copies in the same seconds left one hung and
  one faulted in the renderer's start-up, and the driver reset the GPU
  once under a copy of Advanced Warfare; another round's game was on the
  same GPU. Not seen with two copies.

Screenshots are in `private-captures\allmodes\` (`pc_*` the Windows copies),
logs in `private-logs\allmodes\`.

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
