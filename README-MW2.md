# MW2 Recompiled: Modern Warfare 2 on Android

A native Android port of *Call of Duty: Modern Warfare 2*, recompiled from
**your own Xbox 360 disc**: the campaign, Special Ops and multiplayer in one
app. It is not an emulator and nothing is streamed: the console's code is
translated ahead of time by [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)
and compiled for your device, with a small runtime standing in for the console
(Vulkan graphics, audio, controllers).

> **Unofficial fan project.** Not affiliated with or endorsed by Activision or
> Microsoft. This repository contains no game data, no executables and no code
> made from them. You bring your own disc image; everything built from it stays
> on your own devices. **Never upload or share the game code package, the
> extracted files or the generated code**: they contain the game.

Other games: [Modern Warfare 3](https://github.com/yassinsolim/mw3-recomp)
(also builds for Windows) · [Call of Duty 4](https://github.com/yassinsolim/cod4-recomp)

## What you need

- **Your own *Modern Warfare 2* disc as an `.iso`** (about 7.3 GB). Normal PC
  disc drives can't read Xbox 360 discs, so the dump has to come from a drive
  that can. Only the disc's own version (1.0.557) is accepted; any other is
  refused rather than built wrong.
- **An Android 10 or newer device with a Snapdragon 865 or better**, 8 GB of
  RAM recommended. Tested: AYN Thor and Retroid Pocket Flip 2. Mali GPUs from
  the G57 up are untested; Mali G52 and G76 (and older) are not supported.
- **About 8 GB free** for the disc image, on the device or an SD card, plus
  about 1 GB while the game code is built.

## Quick start (no PC)

1. Install the APK from [Releases](https://github.com/yassinsolim/mw2-recomp/releases/latest)
   and open it.
2. Tap **Build on this device** and pick your `.iso` with the Files app (it is
   only read). The app downloads its build tools once (43 MB); Android asks
   twice to allow installing them.
3. Tap **Build** and wait about **3 to 6 minutes** (3 min on the AYN Thor,
   5 and a half on the Flip 2). You can leave the app or lock the screen; it carries
   on. Keep the device charging: the build uses every core.
4. Tap **Install & Play**. The game plays straight from the same `.iso`;
   nothing is copied.
5. Play: **Campaign · Special Ops** or **Multiplayer**, with the touch overlay
   or a Bluetooth/USB controller.

The app updates itself. When an update needs the game code made again it
says so; tap **Build the game code** (no disc image is asked for).

<details>
<summary><b>Other ways to install</b></summary>

- **Windows PC**: get the code (`git clone https://github.com/yassinsolim/mw2-recomp.git C:\mw2`,
  a short path without spaces), connect the phone over USB with USB debugging
  on, and run

  ```
  setup\setup.cmd -Game mw2 -Iso "D:\Games\MW2.iso" -Install -PushIso
  ```

  It downloads what it needs into `.toolchain\` (only Microsoft's C++ Build
  Tools are installed system-wide, with your leave), builds `out\mw2-game.apk`
  and installs it with the app. After an app update that needs new game code:
  `git pull`, then `setup\setup.cmd -Game mw2 -GameOnly`. `-Check` lists what
  it would do and changes nothing.
- **Linux PC** (tested on Ubuntu 24.04, WSL2 included), with the phone
  connected over USB:

  ```sh
  git clone https://github.com/yassinsolim/mw2-recomp.git && cd mw2-recomp
  bash setup/setup.sh --game mw2 --iso ~/Downloads/MW2.iso --install --push-iso
  ```

  `--game-only` rebuilds the game code after an update.
- **GitHub builds it for you** (no PC, a private repository of your own): not
  available yet.

Every way, step by step: [docs/BUILD-WITHOUT-PC.md](docs/BUILD-WITHOUT-PC.md).
How the on-device build works and what was measured:
[docs/ondevice-build.md](docs/ondevice-build.md).

</details>

## What works

Measured on an AYN Thor (Snapdragon 8 Gen 2, 120 Hz) and a Retroid Pocket
Flip 2 (Snapdragon 865, 60 Hz) with the app's defaults. Other hardware is
untested.

| | | Measured |
|---|---|---|
| Campaign | ✅ | Thor: S.S.D.D. 116 FPS, Team Player 102; Flip: S.S.D.D. 60, Team Player 53 to 59 |
| Special Ops | ✅ | The Pit plays on the Flip; not run on the Thor |
| Multiplayer: System Link | ✅ | Thor and Flip in one match, 119 and 60 FPS; Rust 119 and 60 |
| Multiplayer: Find Match, through the project's server (not Xbox LIVE) | ✅ | Team Deathmatch, Thor and Flip, 119 and 60 FPS |
| Bots in matches your device hosts | ✅ | 0 to 17; six on Terminal on the Thor: 119 to 120 FPS |
| Split screen | ❔ | not tested |
| Title update 6 | ❌ | not in this app |

Sources and methods: [docs/android.md](docs/android.md),
[docs/multiplayer.md](docs/multiplayer.md),
[docs/MW2-ANDROID.md](docs/MW2-ANDROID.md).

## Tips

- **Playing from the ISO.** The game reads its files out of the `.iso` where it
  lies, on the device or an SD card, so keep it there. **Settings → Files**
  copies the game into the app instead (about 7 GB more) and moves it back.
- **AYN Thor stuck at 60 Hz?** Turn on **Settings → Display → Smooth Display**
  in Android, or the refresh-rate tile in the quick settings. The dual-screen
  assistant can turn it off again after a restart.
- **Map packs (DLC), experimental.** Copy a package from your own console into
  `Android/data/com.mw2.recomp/files/dlc/` or a `dlc` folder beside the `.iso`.
  Only a test package has been tried (it played Rust), never a real map pack,
  and packs made for later game versions may not load:
  [docs/dlc.md](docs/dlc.md).
- **Bots.** Tap **Bots** under Multiplayer, then **System Link → Create Game →
  Start Game**; bots only join matches your device hosts. They have no
  routes on the maps, so they feel their way round walls.
- **Settings** are on the app's start screen: network, multiplayer, files,
  graphics. In a game, the pause menu has **Edit controls**.
- **Online** play goes through the project's relay at `mw3.yassin.app:3074`,
  which sees your IP address like any game server
  ([docs/relay.md](docs/relay.md)). **LAN** plays on your own WiFi only.
- **Use the bundled graphics driver** (the default on Qualcomm). With the
  phone's own one, walls and surfaces are missing for the first minutes of
  every level.
- The first visit to a level is slower while its graphics are compiled:
  expect some stutter and now and then a surface missing for a moment. Later
  visits reuse what was compiled.

## Troubleshooting

| Problem | What to do |
|---|---|
| "The game code is not on this device" | Tap **Build on this device** (step 2 above) |
| "The game code does not match this version" | After an update: tap **Build the game code** again |
| "…is not a version of the game this app supports" | The `.iso` is another game or not the disc's own version (1.0.557) |
| The build stopped or failed | What was done is kept: tap **Build** again. If it fails again, the screen says where its report is; share it in an issue, after reading it (it can name files and folders) |
| Stuck at 60 Hz on the AYN Thor | See Tips: Smooth Display |
| Can't connect online, or friends don't see your game | **Settings → Network** on **Online** (restart the game after changing it); everyone on the same app version; the host must have *started* the match |
| "The disc image is not there any more" | Put the `.iso` back (SD card, moved file) or choose it again with **Install & Play** |
| Black screen or crash on a non-Qualcomm phone | Untested hardware; the app's **Log** screen says what happened |
| `setup.cmd` problems | Logs are in `out\logs\`; use a short path such as `C:\mw2`; accept the USB debugging prompt on the phone |

Bug reports are welcome with the log (the app's **Log** screen).

<details>
<summary><b>For developers</b></summary>

This is the same code as [mw3-recomp](https://github.com/yassinsolim/mw3-recomp);
the repositories differ only in which app their releases carry.

- How it works: [docs/MW2-ANDROID.md](docs/MW2-ANDROID.md),
  [docs/ANDROID-ARCHITECTURE.md](docs/ANDROID-ARCHITECTURE.md),
  [docs/runtime.md](docs/runtime.md), [docs/rendering.md](docs/rendering.md),
  [docs/multiplayer.md](docs/multiplayer.md),
  [docs/building.md](docs/building.md).
- The app and the game code are two packages, checked against each other when
  the game starts: [docs/game-library.md](docs/game-library.md). The app
  carries no game code; `tools/check_runtime_apk.py` refuses one that does.
- Asset-free tests: `cmake -S . -B build-tests -G Ninja -DMW2_TESTS_ONLY=ON && cmake --build build-tests && ctest --test-dir build-tests`
- Releasing: [docs/android-releases.md](docs/android-releases.md). Changes:
  [CHANGELOG.md](CHANGELOG.md).
- `tools/check_no_game_files.py` (also `--staged`, as a pre-commit check)
  refuses commits that carry game data. Never commit or publish anything from
  `mw2/`, `ppc_*/`, `out/` or a build folder.

</details>

## Credits

This port stands on the work of others:

- [mw2-recompiled](https://github.com/PaulCombal/mw2-recompiled) by Paul
  Combal and contributors: the runtime, the recompilation pipeline, the relay
  and the online play all of this grew from
- mw2-android, the original Android port by Player124413 and OpenPort-Labs
  (its repository no longer answers), and its fork
  [flavinxs0/mw2-android](https://github.com/flavinxs0/mw2-android) by
  flavinxs0 and contributors: the Android layer
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev): the
  PowerPC recompiler
- [Xenia](https://github.com/xenia-project/xenia): the reference for the
  console's behaviour, and its FFmpeg branch's XMA decoder
- [LLVM](https://github.com/llvm/llvm-project) (clang and lld in the on-device
  build tools) and [AOSP bionic](https://android.googlesource.com/platform/bionic)'s headers
- [libadrenotools](https://github.com/bylaws/libadrenotools) and
  [Mesa Turnip](https://gitlab.freedesktop.org/mesa/mesa) (builds by
  [StevenMXZ](https://github.com/StevenMXZ/Adreno-Tools-Drivers)): the
  graphics driver on Adreno
- [mw3-recomp](https://github.com/yassinsolim/mw3-recomp), this project: the
  renderer and Android work since, the app that updates itself and the game
  code as a package of its own

The full list of credits
([mw3-recomp's README](https://github.com/yassinsolim/mw3-recomp#credits))
and the licences: [THIRD_PARTY.md](THIRD_PARTY.md).

## Licence

The code in this repository is licensed under the
[GNU General Public License v3.0 only](LICENSE), as the projects it derives
from are. Third-party components keep their own licences
([THIRD_PARTY.md](THIRD_PARTY.md)).

## Disclaimer

This is an unofficial fan project. It is not affiliated with, endorsed by or
sponsored by Activision, Infinity Ward or Microsoft. *Call of Duty* and
*Modern Warfare* are trademarks of Activision Publishing, Inc.; *Xbox 360* and
*Xbox LIVE* are trademarks of Microsoft. No game content is distributed here:
you need your own legally obtained copy of the game, and what you build from
it is for your own use only.
