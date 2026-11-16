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
| Android arm64 build (`assembleMw3Tu0Debug`) | verified: 17 MB debug APK, installs on the Thor |
| On-device install from the ISO | verified: title id and hash check pass, 184 files copied |
| Guest code executing on the Thor | verified: start-up, menus and a Survival map load run |
| Menus on the Thor | verified: title, first-run setup, main menu, Special Ops, Survival map list, all at 60 FPS (HUD counter) on the stock Adreno driver. Campaign and Multiplayer thumbnails on the main menu are blurred |
| Survival on the Thor | partially working: Resistance (`so_survival_mp_paris`) loads in about 15 s; models, props, most buildings, the view weapon, enemies, HUD and minimap are drawn. **Open defect, device only:** the sky and the street and other large world surfaces are black (the desktop build draws them). **29-37 FPS** in play (app counter and the runtime's own 32.9 world frames a second; 1024x600 scene at scale 1, title's 2x MSAA drawn at 1x), 59-69 C within two minutes, CPU 30-40% |
| Enemies, AI, damage | observed: enemies spawn, approach and shoot, armour and health fall, last stand triggers |
| Player movement, aiming, shooting | camera look through the touch stick verified; a trigger hold shows the weapon in recoil and a stick push changes the view, but neither firing nor movement is confirmed beyond that; nobody has played a wave by hand |
| Stability | one abort two minutes into a match (`std::unordered_map` growth inside `kernel::ObjectAt` from `RtlLeaveCriticalSection`), before the merge shortcuts were undone; not seen in the dozen one-to-two-minute matches since. The Turnip T26 driver crashes inside the driver at renderer start-up (stock driver is what works) |
| Audio on the Thor | AAudio stream opens, XMA contexts decode; audible output not checked by ear |
| Input on the Thor | verified for menus through the touch overlay (a press must be held about 0.25 s); physical pad detected, not exercised |
| Launcher branding | verified: MW3 wordmark, MW2-only entries hidden |
| Profile and saves | profile settings are written; a completed first-run setup is remembered |

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
- In play the title sends about 6,500 draws a frame (it renders the scene in
  two EDRAM tiles, replaying its command list for each); the ring consumer is
  busy 43% of the run.

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
