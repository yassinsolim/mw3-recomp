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
| Bots in your matches (the launcher's **Bots** entry, 0 to 17) | join, fight and score | same (`MW2_BOTS`) |
| Voice chat (off by default; push-to-talk) | built, not yet tested | built, not yet tested |
| Everything unlocked in multiplayer, with fifteen custom classes | yes, chosen camos show in matches | yes |
| Extra camouflages below Gold in multiplayer ([packs](docs/textures.md#camouflage-packs)) | works | works |
| Custom skins (texture replacement, animated) | works | should work, not tested |
| Audio | works | not checked |
| Controls | touch overlay, Bluetooth/USB controllers | controller, keyboard and mouse |
| Saves and profile | work | not checked |

The detailed log is [docs/ANDROID-PROGRESS.md](docs/ANDROID-PROGRESS.md).

*Modern Warfare 2*, which this port grew from, builds from its own disc the
same way (`setup\setup.cmd -Game mw2`, Android only): one app with the
campaign, Special Ops and the multiplayer that updates itself from
[mw2-recomp](https://github.com/yassinsolim/mw2-recomp)'s releases, and the
game code as your own package. It shares every renderer fix made since
([docs/MW2-ANDROID.md](docs/MW2-ANDROID.md), [README-MW2.md](README-MW2.md)):

| Modern Warfare 2 (disc version) | AYN Thor | Retroid Pocket Flip 2 (Snapdragon 865) |
|---|---|---|
| Campaign | plays (S.S.D.D. at 60 FPS, Team Player at about 57) | plays at 30 FPS |
| Multiplayer, System Link | plays (Terminal and Rust at 60 FPS) | plays at 30 FPS |
| Special Ops | not tested | The Pit plays |
| Split screen, two devices in a match, title update 6 | not tested | not tested |

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
3. Answer its questions: Android and/or PC, which part for PC (campaign,
   multiplayer or both), and where your `.iso` is.

That is all. The script downloads the tools it needs into `.toolchain\` (a
JDK, the Android SDK and NDK, LLVM, CMake, Ninja, and Python and Git if you
have none; each pinned and checked by SHA-256), reads the executables out of
your disc image (read-only), checks them, recompiles them and builds:

- `out\mw3-game.apk`: the game code for Android (`com.mw3.recomp.game`), made
  from your disc: the campaign's, Special Ops' and the multiplayer's. It has
  no screen of its own and is yours alone: never share it.
- `out\pc\`: the Windows game, if you asked for it

and downloads the Android app itself (`com.mw3.recomp`) from this project's
[releases](https://github.com/yassinsolim/mw3-recomp/releases). The app
contains none of the game; it finds the game code you built beside it, and
**updates itself** from then on: it looks for a newer release when it starts
(and from **Check for updates**), shows what changed, and installs it over
itself. Your game files, saves and game code stay as they are
([docs/game-library.md](docs/game-library.md)).

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
| `-Title both\|sp\|mp` | which executable(s) for PC (the Android app always holds both) |
| `-Build android\|pc\|both` | what to build |
| `-Install` | install the APK on the connected phone without asking |
| `-PushIso` | also copy the disc image to the phone's Download folder |
| `-Recomp` | recompile again (setup does so by itself when an update changed the recompiler or its configuration) |
| `-GameOnly` | Android: build and install the game code only, and leave the app alone |
| `-Dev` | Android: build the app here instead of downloading the published one (`-Suffix .dev` puts it beside that one) |
| `-Yes` | ask nothing, take the defaults |

Running it again is safe: finished steps are skipped.

**After an update of the app**, nothing is needed most of the time. Now and
then an update changes how the game is recompiled, and the app then says that
the game code on the phone does not match it: on the PC, `git pull`, then
`setup\setup.cmd -GameOnly` with the phone connected. It rebuilds the game
code (minutes, not hours) and installs it; game files and saves are kept.

## Install on Android

1. **The game code and the app.** With the phone connected over USB and
   [USB debugging](https://developer.android.com/studio/debug/dev-options)
   on, setup offers to install both. Or copy the two APKs to the phone and
   open each (allow installing from that app when Android asks). A phone
   that still has an app built on a PC by an earlier setup: Android will not
   put the published app over it (they are signed with different keys), so
   setup offers to replace it, keeping the game files and saves; the app's
   settings start afresh.
2. **The game files.** Copy your `.iso` to the phone (setup can do it:
   `-PushIso`, or answer yes). Open the app, choose **Install the game files**,
   then **Browse the phone's storage** and pick the image. The app checks
   both executables and copies what it needs into its own storage (about
   7 GB), once: the campaign and the multiplayer read the same folder. From
   then on the app's start screen shows the game's own artwork, read from
   your disc's files on the phone: the app is published without it.
3. **Play.** Tap **Campaign · Special Ops** or **Multiplayer**. The game's
   own menu entries that go from one to the other work too: the app restarts
   the game with the other half. The on-screen pad can be edited from the
   pause menu (**Edit controls**); a Bluetooth or USB controller works too.

Coming from the two separate apps: the new app installs over the campaign app
and keeps its saves and game files; run **Install the game files** once more
so the multiplayer's executable is copied too (only what is missing is
copied). Setup copies the old multiplayer app's profile and rank across over
USB when it installs; after that the old "CoD: MW3 Multiplayer" app can be
uninstalled, which frees its copy of the game files.

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

## Bots and unlocks (multiplayer)

- **Bots.** On the app's start screen, tap **Bots** (under **Multiplayer**)
  and choose how many and how good; the entry then reads `BOTS · 6 · REGULAR`.
  Start **Multiplayer**, then **System Link → Create Game → Start Game** (or a
  private match): the bots join the matches *your* device hosts, and leave
  to make room when a player joins. They do nothing in a match somebody else
  hosts. In a match, the app's pause menu has **Bots in my matches** to change
  the number without leaving. The same settings and the rest (all bots
  against the players, map vote, match options) are in **Settings →
  Multiplayer**. On the PC: `MW2_BOTS=6` in `.env`.
- **Everything unlocked.** On by default (**Settings → Multiplayer → Unlock
  all camos and items**; `MW2_UNLOCK_ALL=1` on the PC): every weapon,
  attachment, proficiency, camouflage, reticle, perk, piece of equipment,
  death streak and strike package streak, every title and emblem, and
  fifteen custom classes, from rank 1. It is your copy reading your stats as
  unlocked: nothing is written to them, and your rank, prestige and weapon
  levels stay what you earned. Other players see the class you built; a
  match hosted by a copy without the switch gives you only what your real
  levels allow. Prestige Mode and its double experience are left as earned
  ([details](docs/MW3-MP-BRINGUP.md#everything-unlocked)).

## Linux

The same pipeline runs on Linux (Ubuntu 24.04, also under WSL2), by hand:

```sh
python3 tools/xdvdfs.py <your.iso> mw3/tu0 default.xex default_mp.xex   # read-only
bash tools/recomp_mw3.sh       # campaign    -> ppc_mw3/
bash tools/recomp_mw3.sh mp    # multiplayer -> ppc_mw3_mp/
for title in mw3 mw3mp; do
  MW2_TITLE=$title MW2_VERSION=tu0 python3 tools/genshared.py
  MW2_TITLE=$title MW2_VERSION=tu0 python3 tools/gen_kernel_stubs.py
done
bash android/fetch_deps.sh     # libadrenotools and the Turnip driver
cd android && echo "sdk.dir=$ANDROID_HOME" > local.properties
MW2_LTO=OFF ./gradlew assembleMw3Tu0Debug     # one APK, both titles
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
| the app says the game files are missing | do step 2 of *Install on Android* |
| the app says the game code is not on this device | run `setup\setup.cmd` with the phone connected: it builds and installs it |
| the app says the game code does not match this version | on the PC: `git pull`, then `setup\setup.cmd -GameOnly` with the phone connected |
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

The Android app and the game code are two packages, checked against each
other when the game starts: [docs/game-library.md](docs/game-library.md).
Releasing the app: [docs/android-releases.md](docs/android-releases.md).

`tools/check_no_game_files.py` (also `--staged`, as a pre-commit check)
refuses commits that carry game data, and `tools/check_runtime_apk.py`
refuses an APK that carries recompiled code. Never commit or publish anything
from `mw3/`, `ppc_*/`, `out/` or a build folder.

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
