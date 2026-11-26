# CoD4 Recompiled: Call of Duty 4: Modern Warfare on Android

A native Android port of *Call of Duty 4: Modern Warfare*, recompiled from
**your own Xbox 360 disc**: the campaign and multiplayer in one app. It is not
an emulator and nothing is streamed: the console's code is translated ahead of
time by [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and compiled
for your device, with a small runtime standing in for the console (Vulkan
graphics, audio, controllers). It is the newest of the three ports and the
least played: expect rough edges.

> **Unofficial fan project.** Not affiliated with or endorsed by Activision or
> Microsoft. This repository contains no game data, no executables and no code
> made from them. You bring your own disc image; everything built from it stays
> on your own devices. **Never upload or share the game code package, the
> extracted files or the generated code**: they contain the game.

Other games: [Modern Warfare 3](https://github.com/yassinsolim/mw3-recomp)
(also builds for Windows) · [Modern Warfare 2](https://github.com/yassinsolim/mw2-recomp) ·
[Advanced Warfare](https://github.com/yassinsolim/aw-recomp)

## What you need

- **Your own *Call of Duty 4: Modern Warfare* disc as an `.iso`** (about
  7.8 GB). Normal PC disc drives can't read Xbox 360 discs, so the dump has to
  come from a drive that can. Only the USA/Europe disc (2007) is accepted; any
  other is refused rather than built wrong.
- **An Android 10 or newer device with a Snapdragon 865 or better**, 8 GB of
  RAM recommended. Tested: AYN Thor and Retroid Pocket Flip 2. Mali GPUs from
  the G57 up are untested; Mali G52 and G76 (and older) are not supported.
- **About 8 GB free** for the disc image, on the device or an SD card, plus
  about 1 GB while the game code is built.

## Quick start (no PC)

1. Install the APK from [Releases](https://github.com/yassinsolim/cod4-recomp/releases/latest)
   and open it.
2. Tap **Build on this device**. An `.iso` on the device or its SD card is
   found by itself where Android lets the app look; otherwise pick it with the Files app, or browse
   to it and tap it (or **Use this folder** in the folder it is in). It is
   only read. The app downloads its build tools once (43 MB); Android asks
   twice to allow installing them.
3. Tap **Build** and wait about **2 to 4 minutes** (2 min 12 s on the AYN
   Thor, 3 min 49 s on the Flip 2). You can leave the app or lock the screen;
   it carries on. Keep the device charging: the build uses every core.
4. Tap **Install & Play**. The game plays straight from the same `.iso`;
   nothing is copied.
5. Play: **Campaign** or **Multiplayer**, with the touch overlay or a
   Bluetooth/USB controller.

The app updates itself. When an update needs the game code made again it
says so; tap **Build the game code** (no disc image is asked for).

<details>
<summary><b>Other ways to install</b></summary>

- **Windows PC**: get the code (`git clone https://github.com/yassinsolim/cod4-recomp.git C:\cod4`,
  a short path without spaces), connect the phone over USB with USB debugging
  on, and run

  ```
  setup\setup.cmd -Game cod4 -Iso "D:\Games\CoD4.iso" -Install -PushIso
  ```

  It downloads what it needs into `.toolchain\` (only Microsoft's C++ Build
  Tools are installed system-wide, with your leave), builds `out\cod4-game.apk`
  and installs it with the app. `-Build pc` or `-Build both` makes the Windows
  game as well, in `out\pc-cod4`. After an app update that needs new game
  code: `git pull`, then `setup\setup.cmd -Game cod4 -GameOnly`. `-Check` lists
  what it would do and changes nothing.
- **Linux PC** (tested on Ubuntu 24.04, WSL2 included), with the phone
  connected over USB:

  ```sh
  git clone https://github.com/yassinsolim/cod4-recomp.git && cd cod4-recomp
  bash setup/setup.sh --game cod4 --iso ~/Downloads/CoD4.iso --install --push-iso
  ```

  `--game-only` rebuilds the game code after an update.
- **GitHub builds it for you** (no PC, a private repository of your own): not
  available yet.

Every way, step by step: [docs/BUILD-WITHOUT-PC.md](docs/BUILD-WITHOUT-PC.md).
How the on-device build works and what was measured:
[docs/ondevice-build.md](docs/ondevice-build.md).

</details>

## What works

Measured on 23 November 2026 on an AYN Thor (Snapdragon 8 Gen 2, 120 Hz) and a
Retroid Pocket Flip 2 (Snapdragon 865, 60 Hz) with the app's defaults. Other
hardware is untested.

| | | Measured |
|---|---|---|
| Campaign | ✅ | F.N.G. 119 FPS on the Thor, 60 on the Flip; Crew Expendable 119 and 60; Charlie Don't Surf 117 and 59.5 |
| Multiplayer: System Link | ✅ | Thor and Flip in one match on Backlot, 119 and 60 FPS; Crash 119 and 60 |
| Multiplayer: Find Match, through the project's server (not Xbox LIVE) | ✅ | Team Deathmatch and Free-for-all, Thor and Flip, 119 and 60 FPS |
| Bots in matches your device hosts | ✅ | six on Crash on the Flip for ten minutes, 60 FPS; not run on the Thor |
| Checkpoints on Android | ❔ | work on Windows; not checked on Android |
| Split screen | ⚠️ | on Windows, with two controllers: a Team Deathmatch on Backlot plays. Flip 2, the second controller simulated (none was at hand): the same at 60 FPS |
| Xbox LIVE invitations | ❔ | not tested |

Only three campaign levels and two multiplayer maps have been played for more
than a few minutes; the rest load from the same code but were not checked one
by one. Charlie Don't Surf stutters where the helicopters land. What has been
checked and what is open: [docs/COD4-BRINGUP.md](docs/COD4-BRINGUP.md).

## Tips

- **Playing from the ISO.** The game reads its files out of the `.iso` where it
  lies, on the device or an SD card, so keep it there. **Settings → Files**
  copies the game into the app instead (about 7 GB more) and moves it back.
- **AYN Thor stuck at 60 Hz?** Turn on **Settings → Display → Smooth Display**
  in Android, or the refresh-rate tile in the quick settings. The dual-screen
  assistant can turn it off again after a restart.
- **Your own map packs (DLC), untested with a real pack.** Copy
  `Content\0000000000000000\415607E6\00000002` off your Xbox 360's hard
  disk, then **Add DLC → Choose a folder** in the app. Only a test package
  made from the disc has been tried (it played Shipment), never the Variety
  Map Pack, whose maps the disc's menus do not list. Online, every player
  needs the map's pack: [docs/dlc.md](docs/dlc.md).
- **Bots.** Tap **Bots** under Multiplayer, then host a **System Link** game;
  bots only join matches your device hosts.
- **Settings** are on the app's start screen: network, multiplayer, files,
  graphics. In a game, the pause menu has **Edit controls**.
- **Online** play goes through the project's relay at `mw3.yassin.app:3074`,
  which sees your IP address like any game server
  ([docs/relay.md](docs/relay.md)). **LAN** plays on your own WiFi only.
- **Sun shadows** are off above 60 FPS: with them the Thor drops to 114 to 115
  with many late frames.
- The first visit to a level may stutter while its graphics are compiled;
  later visits reuse what was.

## Troubleshooting

| Problem | What to do |
|---|---|
| "The game code is not on this device" | Tap **Build on this device** (step 2 above) |
| "The game code does not match this version" | After an update: tap **Build the game code** again |
| "…is not a version of the game this app supports" | The `.iso` is another game or not the USA/Europe disc this port was made for |
| The build stopped or failed | What was done is kept: tap **Build** again. If it fails again, the screen says where its report is; share it in an issue, after reading it (it can name files and folders) |
| Stuck at 60 Hz on the AYN Thor | See Tips: Smooth Display |
| Can't connect online, or friends don't see your game | **Settings → Network** on **Online** (restart the game after changing it); everyone on the same app version; the host must have *started* the match |
| System Link: the other device's game is not listed, or the overlay says `LAN: 0` | Both devices must be on the same Wi-Fi network, behind the same router. With two routers, a mesh's second network or a guest network they cannot hear each other: set **Settings → Network** to **Online** on both (restart the game) |
| "The disc image is not there any more" | Put the `.iso` back (SD card, moved file) or choose it again with **Install & Play** |
| Black screen or crash on a non-Qualcomm phone | Untested hardware; the app's **Log** screen says what happened |
| `setup.cmd` problems | Logs are in `out\logs\`; use a short path such as `C:\cod4`; accept the USB debugging prompt on the phone |

Bug reports are welcome with the log (the app's **Log** screen).

<details>
<summary><b>For developers</b></summary>

This is the same code as [mw3-recomp](https://github.com/yassinsolim/mw3-recomp);
the repositories differ only in which app their releases carry.

- How it works: [docs/COD4-BRINGUP.md](docs/COD4-BRINGUP.md),
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
  `cod4/`, `ppc_*/`, `out/` or a build folder.

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
- [mw3recomp](https://github.com/the-drk/mw3recomp) by DrK: the *Modern
  Warfare 3* desktop bring-up this runtime also carries
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
