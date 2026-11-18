# MW3 Recompiled: Modern Warfare 3 on Android and PC

*Call of Duty: Modern Warfare 3* (2011) for the Xbox 360, turned into a native
Android app and a native Windows game. The console's PowerPC code is
translated ahead of time into C++ by
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and compiled for your
phone or PC, with a runtime that stands in for the console: its kernel, the
GPU (drawn with Vulkan), audio and controllers. It is not an emulator, and
there is no streaming.

> **You need your own copy of the game.** This repository contains no game
> data, no executables and no code made from them. You supply a disc image
> (`.iso`) of your own Xbox 360 *Modern Warfare 3* disc; the build reads it on
> your PC and everything made from it stays on your own devices. **Never upload
> or share the APKs, the Windows game, the extracted files or the generated
> code** (`out/`, `mw3/`, `ppc_mw3*/`): they contain the game.

## Status

Tested mostly on one Android device (an AYN Thor: Snapdragon 8 Gen 2,
Adreno 740, Android 13) and one Windows PC. Other hardware is untested.

| | Android | Windows |
|---|---|---|
| Campaign | plays (Black Tuesday at about 58 FPS) | builds; not yet tested on Windows |
| Special Ops Survival | plays at about 60 FPS | builds; not yet tested on Windows |
| Multiplayer, split screen | plays (Seatown at 60 FPS) | not tested |
| Multiplayer online, System Link over the relay | works (Android to PC tested) | works (joined an Android host) |
| Xbox LIVE menu (Find Match, served by this project, not Microsoft or Activision) | signs in, lists playlists, opens a public lobby; a full match not yet tested | two copies matched through Find Match |
| Bots in your matches (Settings, 0 to 17) | join, fight and score | same |
| Voice chat (off by default; push-to-talk) | built, not yet tested | built, not yet tested |
| Everything unlocked in multiplayer | yes, chosen camos show in matches | yes |
| Custom skins (texture replacement, animated) | works | should work, not tested |
| Audio | works | not checked |
| Controls | touch overlay, Bluetooth/USB controllers | controller, keyboard and mouse |
| Saves and profile | work | not checked |

The detailed log is [docs/ANDROID-PROGRESS.md](docs/ANDROID-PROGRESS.md).

## What you need

- Your own Xbox 360 *Modern Warfare 3* disc, dumped to an `.iso`. The build
  checks the executables on it and accepts only the disc builds it has been
  made for (the USA/Europe disc and one other packaging of the campaign;
  [tools/check_mw3_xex.py](tools/check_mw3_xex.py)). Others are refused rather
  than built wrong.
- A Windows 10 or 11 PC (64-bit) with 16 GB of memory or more and about 40 GB
  free (60 GB with the Windows game). A first build takes one to several
  hours.
- For Android: a 64-bit phone or handheld with Android 10 or later and
  Vulkan, about 7 GB free per app for the game files (plus room for the
  ~8 GB disc image while installing). Qualcomm (Adreno) devices get a bundled
  Turnip driver; others use their own driver and are untested.
- For the Windows game: a GPU with Vulkan and a processor from 2011 or later
  (x86-64-v2).

## Quick start (Windows)

1. Get the code: `git clone https://github.com/yassinsolim/mw3-recomp.git C:\mw3`
   (or **Code → Download ZIP** and unpack it). A short path without spaces,
   such as `C:\mw3`, avoids path-length trouble.
2. Double-click `setup\setup.cmd` (or run it from a terminal).
3. Answer its questions: which part (campaign, multiplayer or both), Android
   and/or PC, and where your `.iso` is.

That is all. The script downloads the tools it needs into `.toolchain\` (a
JDK, the Android SDK and NDK, LLVM, CMake, Ninja, and Python and Git if you
have none; each pinned and checked by SHA-256), reads the executables out of
your disc image (read-only), checks them, recompiles them and builds:

- `out\mw3-debug.apk`: campaign and Special Ops (`com.mw3.recomp`)
- `out\mw3mp-debug.apk`: multiplayer (`com.mw3.recomp.mp`)
- `out\pc\`: the Windows game, if you asked for it

The one thing it cannot fetch by itself is Microsoft's C++ Build Tools, which
the recompiler is built with. If they are missing it offers to install them
with `winget` (about 3 GB, needs administrator rights); otherwise install
[Build Tools for Visual Studio](https://visualstudio.microsoft.com/downloads/)
with **Desktop development with C++** and run setup again.

Useful options (`setup\setup.cmd -<option>`):

| option | what it does |
|---|---|
| `-Check` | read-only: lists what is present and what would be downloaded, changes nothing |
| `-Iso <path>` | the disc image, instead of being asked |
| `-Title both\|sp\|mp` | which executable(s) |
| `-Build android\|pc\|both` | what to build |
| `-Install` | install the APKs on the connected phone without asking |
| `-PushIso` | also copy the disc image to the phone's Download folder |
| `-Recomp` | recompile again, e.g. after updating the code |
| `-Yes` | ask nothing, take the defaults |

Running it again is safe: finished steps are skipped. After `git pull`, run it
again to rebuild; add `-Recomp` if the release notes or commits mention the
recompiler, `config/` or `patches/`.

## Install on Android

1. **The app.** With the phone connected over USB and
   [USB debugging](https://developer.android.com/studio/debug/dev-options)
   on, setup offers to install the APKs. Or copy the APK to the phone and open
   it (allow installing from that app when Android asks).
2. **The game files.** Copy your `.iso` to the phone (setup can do it:
   `-PushIso`, or answer yes). Open the app, choose **Install the game files**,
   then **Browse the phone's storage** and pick the image. The app checks it
   and copies what it needs into its own storage (about 7 GB). The campaign
   and multiplayer apps each install their own copy.
3. **Play.** Tap **Play**. The on-screen pad can be edited from the pause
   menu (**Edit controls**); a Bluetooth or USB controller works too.

## Play on PC

Start `out\pc\Multiplayer.cmd` or `out\pc\Campaign.cmd`. Settings live in
`out\pc\.env`, one `NAME=value` a line (all of them:
[docs/switches.md](docs/switches.md)); setup writes one with your player name,
the public relay and a 1280×720 window (`MW2_FULLSCREEN=1` for full screen,
F8 toggles it). The log is `out\pc\mw3.log`.

## Online play

The multiplayer meets other players through a small relay server, which
carries the game's **System Link** traffic across the internet. A public one
runs at **`mw3.yassin.app:3074`** (UDP) and is the default: the Android app's
Settings → Network is **Online** with that address, and the PC's `.env` has
`MW2_NETWORK=server` and `MW2_SERVER=mw3.yassin.app:3074`.

- In the game choose **MULTIPLAYER → SYSTEM LINK**. One player creates a game
  and **starts the match**; it then appears in everyone else's list.
- **Xbox LIVE does not work** (see Status). Use System Link.
- Play with copies built from the same version of this repository; different
  versions may refuse each other or misbehave.
- The relay sees your IP address, like any game server. It needs no account
  or password. **LAN** in the app's settings (or `MW2_NETWORK=lan`) plays on
  your own network only.
- You can run your own relay: [docs/relay.md](docs/relay.md).

## Linux

The same pipeline runs on Linux (Ubuntu 24.04, also under WSL2), by hand:

```sh
python3 tools/xdvdfs.py <your.iso> mw3/tu0 default.xex default_mp.xex   # read-only
bash tools/recomp_mw3.sh       # campaign  -> ppc_mw3/   (tools/recomp_mw3.sh mp: multiplayer)
MW2_TITLE=mw3 MW2_VERSION=tu0 python3 tools/genshared.py
MW2_TITLE=mw3 MW2_VERSION=tu0 python3 tools/gen_kernel_stubs.py
bash android/fetch_deps.sh     # libadrenotools and the Turnip driver
cd android && echo "sdk.dir=$ANDROID_HOME" > local.properties
MW2_LTO=OFF ./gradlew assembleMw3Tu0Debug     # assembleMw3mpTu0Debug for multiplayer
```

It needs JDK 17, NDK 28.2.13676358, `platforms;android-35`, CMake 3.22.1+,
clang-18, Ninja and Python 3.10+. `build.sh` at the root is the Modern Warfare 2
pipeline this port grew from; for MW3 use the steps above
([docs/ANDROID-PROGRESS.md](docs/ANDROID-PROGRESS.md#build-wsl2-ubuntu-2404),
desktop runs: [docs/MW3-LOCAL-RUN.md](docs/MW3-LOCAL-RUN.md)).

## Troubleshooting

| problem | what to do |
|---|---|
| "Unsupported MW3 executable" | your disc is a build this port has not been made for; only the ones in `tools/check_mw3_xex.py` are accepted |
| setup stops at the C++ Build Tools | install them by hand (see Quick start) and run setup again |
| the build fails or the PC runs out of memory | close other programs and run setup again; it carries on where it stopped. Logs are in `out\logs\` |
| errors about long paths or spaces | move the folder to a short path such as `C:\mw3` |
| `adb` says *unauthorized* | unlock the phone and accept the USB debugging prompt |
| the app says the game files are missing | do step 2 of *Install on Android*; each app needs its own install |
| black screen or crashes on a non-Qualcomm phone | untested hardware; the app's **Log** screen says what happened |
| friends do not see your game | everyone on the same relay, same version; the host must have **started** the match |

Bug reports are welcome with the log (the app's **Log** screen, or
`out\pc\mw3.log`). Read it first: it can name files and folders on your
machine.

## For developers

How the port works and how it was brought up:
[docs/ANDROID-ARCHITECTURE.md](docs/ANDROID-ARCHITECTURE.md),
[docs/MW3-BRINGUP.md](docs/MW3-BRINGUP.md),
[docs/MW3-MP-BRINGUP.md](docs/MW3-MP-BRINGUP.md),
[docs/runtime.md](docs/runtime.md), [docs/rendering.md](docs/rendering.md),
[docs/audio.md](docs/audio.md), [docs/multiplayer.md](docs/multiplayer.md),
[docs/building.md](docs/building.md). Guest addresses per title are in
`runtime/title.h`; recompiler settings in `config/MW3.toml` and
`config/MW3MP.toml`. The asset-free tests run with
`cmake -S . -B build-tests -G Ninja -DMW2_TESTS_ONLY=ON && cmake --build build-tests && ctest --test-dir build-tests`.

`tools/check_no_game_files.py` (also `--staged`, as a pre-commit check)
refuses commits that carry game data. Never commit anything from `mw3/`,
`ppc_*/`, `out/` or a build folder.

## Credits

This port stands on the work of others:

- [mw2-recompiled](https://github.com/PaulCombal/mw2-recompiled) by Paul
  Combal and contributors: the runtime, the recompilation pipeline, the relay
  and the online play this grew from
- [mw3recomp](https://github.com/the-drk/mw3recomp) by DrK: the MW3 desktop
  bring-up
- [mw2-android](https://github.com/OpenPort-Labs/mw2-android) and its fork
  [flavinxs0/mw2-android](https://github.com/flavinxs0/mw2-android)
  (GuruMachanica, flavinxs0 and contributors): the Android layer
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev): the
  PowerPC recompiler
- [Xenia](https://github.com/xenia-project/xenia): the reference for the
  console's behaviour, and its FFmpeg branch's XMA decoder
- [OpenAssetTools](https://github.com/Laupetin/OpenAssetTools) and
  [pc2xbox360mw2ools](https://github.com/slanginbeans/pc2xbox360mw2ools): zone
  structure definitions
- [libadrenotools](https://github.com/bylaws/libadrenotools) and
  [Mesa Turnip](https://gitlab.freedesktop.org/mesa/mesa) (builds by
  [StevenMXZ](https://github.com/StevenMXZ/Adreno-Tools-Drivers)): custom GPU
  drivers on Adreno
- [SDL](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui),
  [zlib](https://github.com/madler/zlib), [FFmpeg](https://ffmpeg.org/),
  [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers), the
  [RenderDoc](https://github.com/baldurk/renderdoc) API header, and the Barlow
  and Michroma fonts
- [LibertyRecomp](https://github.com/OZORDI/LibertyRecomp) and
  [gta4-ps4](https://github.com/plastikman/gta4-ps4): related recompilation work

Their licences: [THIRD_PARTY.md](THIRD_PARTY.md).

## Licence

The code in this repository is licensed under the
[GNU General Public License v3.0 only](LICENSE), as the projects it derives
from are. Third-party components keep their own licences
([THIRD_PARTY.md](THIRD_PARTY.md)).

## Disclaimer

This is an unofficial fan project. It is not affiliated with, endorsed by or
sponsored by Activision, Infinity Ward, Sledgehammer Games or Microsoft.
*Call of Duty* and *Modern Warfare* are trademarks of Activision Publishing,
Inc.; *Xbox 360* and *Xbox LIVE* are trademarks of Microsoft. No game content
is distributed here: you need your own legally obtained copy of the game, and
what you build from it is for your own use only.
