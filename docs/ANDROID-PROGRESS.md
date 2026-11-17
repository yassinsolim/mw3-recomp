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
| Android arm64 build (`assembleMw3Tu0Debug`) | verified: debug APK (27 MB with the bundled driver), installs on the Thor; builds natively on Windows (`scripts/win_build.ps1`, outside the repo) as well as in WSL |
| On-device install from the ISO | verified: title id and hash check pass, 184 files copied |
| Guest code executing on the Thor | verified: start-up, menus, Survival and the campaign (Black Tuesday) run |
| Graphics driver | **Turnip v27.0.0-R2 is the default** on Qualcomm devices (bundled, downloaded at build time). On the Thor's campaign helicopter scene: 51 FPS and clean textures on Turnip against 21 FPS with blocky, missing texture blocks on the stock Qualcomm driver. On Turnip, whole pipelines are built, not pipeline libraries (its compiler faulted building libraries) |
| Menus on the Thor | verified: title, first-run setup, main menu, Special Ops, Survival map list at 60 FPS; no flicker (black frames 0% in a 7 s recording, was 40%) |
| Survival on the Thor | working, slow: Resistance loads in about 15 s; sky, street, buildings, weapon, enemies, HUD and minimap drawn and textured. 10-26 FPS on Turnip in the first minute, the low end while shaders compile on first sight |
| Campaign on the Thor | Black Tuesday resumes from a save and plays; 32-51 FPS on Turnip; four minutes without fault |
| Enemies, AI, damage | observed: enemies spawn, approach and shoot, armour and health fall, last stand triggers |
| Player movement, aiming, shooting | verified with the touch overlay: the left stick moves the player, the right stick turns the view, the fire button fires (muzzle flash and recoil). A full wave has not been played by hand |
| Stability | Turnip with pipeline libraries crashed in its compiler (twice); whole pipelines ran four minutes clean. Turnip T26 crashes at renderer start-up; T30 untested. One old abort in `kernel::ObjectAt` not seen since |
| Audio on the Thor | AAudio stream opens, XMA contexts decode; audible output not checked by ear |
| Input on the Thor | touch overlay verified in menus and play (a press must be held about 0.25 s); physical pad detected, not exercised |
| Launcher branding | verified: MW3 wordmark, MW2-only entries hidden |
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
- Drivers, same Survival run (GPU ms a world frame / world FPS): faux123
  v1.2.10 38.7/25.5, v27.0.0-R2 38.9/25.3, Banners 2026-11-17 39.2/25.2,
  mainline V32 39.4/25.0, MrPurple T30 39.8/24.8. All within 3%; R2 stays
  the default. A driver cannot buy the factor of two the per-draw cost needs.
- Render scale 2 is not hung: its first launch compiles about 624 pipelines
  (72 s) before play, which the hang detector reports as hangs. It then
  plays at 18 FPS (GPU 54.5 ms a frame).

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
