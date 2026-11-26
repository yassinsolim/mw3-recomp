# MW3 Recompiled: Modern Warfare 3 on Android and PC

A native Android (and Windows PC) port of *Call of Duty: Modern Warfare 3*,
recompiled from **your own Xbox 360 disc**. It is not an emulator and nothing
is streamed: the console's code is translated ahead of time by
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and compiled for your
device, with a small runtime standing in for the console (Vulkan graphics,
audio, controllers).

> **Unofficial fan project.** Not affiliated with or endorsed by Activision or
> Microsoft. This repository contains no game data, no executables and no code
> made from them. You bring your own disc image; everything built from it stays
> on your own devices. **Never upload or share the APKs, the extracted files or
> the generated code**: they contain the game.

Other games: [Modern Warfare 2](https://github.com/yassinsolim/mw2-recomp) ·
[Call of Duty 4](https://github.com/yassinsolim/cod4-recomp) ·
[Advanced Warfare](https://github.com/yassinsolim/aw-recomp)

## Official downloads

- The only official APKs are on this project's [GitHub releases page](https://github.com/yassinsolim/mw3-recomp/releases).
- APKs re-uploaded elsewhere (file hosts, Reddit, Discord mirrors) are not ours. They may be modified or out of date, so we can't help with problems they cause.
- The game is never provided. Build it from your own disc image, dumped from a disc you own. We don't link to download sites, and we can't help with downloaded copies.
- Report problems through [GitHub issues](https://github.com/yassinsolim/mw3-recomp/issues), with Log -> Share attached.

## What you need

- **Your own *Modern Warfare 3* disc as an `.iso`** (about 8 GB). Normal PC
  disc drives can't read Xbox 360 discs, so the dump has to come from a drive
  that can. The USA/Europe disc and one other packaging of the campaign are
  accepted; any other is refused rather than built wrong.
- **An Android 10 or newer device with a Snapdragon 865 or better**, 8 GB of
  RAM recommended. Tested: AYN Thor and Retroid Pocket Flip 2. Mali GPUs from
  the G57 up are untested; Mali G52 and G76 (and older) are not supported.
- **About 8 GB free** for the disc image, on the device or an SD card, plus
  about 1 GB while the game code is built.

## Quick start (no PC)

1. Install the APK from [Releases](https://github.com/yassinsolim/mw3-recomp/releases/latest)
   and open it.
2. Tap **Build on this device**. An `.iso` on the device or its SD card is
   found by itself where Android lets the app look; otherwise pick it with the Files app, or browse
   to it and tap it (or **Use this folder** in the folder it is in). It is
   only read. The app downloads its build tools once (43 MB); Android asks
   twice to allow installing them.
3. Tap **Build** and wait about **5 to 8 minutes** (4 min 40 s on the AYN
   Thor, 8 min on the Flip 2). You can leave the app or lock the screen; it carries
   on. Keep the device charging: the build uses every core.
4. Tap **Install & Play**. The game plays straight from the same `.iso`;
   nothing is copied.
5. Play: **Campaign · Special Ops** or **Multiplayer**, with the touch overlay
   or a Bluetooth/USB controller.

The app updates itself. When an update needs the game code made again it
says so; tap **Build the game code** (no disc image is asked for).

<details>
<summary><b>Other ways to install</b></summary>

- **Windows PC**: get the code (`git clone https://github.com/yassinsolim/mw3-recomp.git C:\mw3`,
  a short path without spaces), double-click `setup\setup.cmd` and answer its
  questions (Android and/or PC, where your `.iso` is). It downloads what it
  needs into `.toolchain\`, builds `out\mw3-game.apk` and installs it with the
  app on a phone connected over USB with USB debugging on. For the Windows game
  too, choose PC: start `out\pc\Campaign.cmd` or `out\pc\Multiplayer.cmd`
  (settings in `out\pc\.env`, all of them in [docs/switches.md](docs/switches.md)).
  Microsoft's C++ Build Tools are needed; setup offers to install them.
  After an app update that needs new game code: `git pull`, then
  `setup\setup.cmd -GameOnly`. `-Check` lists what it would do and changes
  nothing.
- **Linux PC** (tested on Ubuntu 24.04, WSL2 included):

  ```sh
  git clone https://github.com/yassinsolim/mw3-recomp.git && cd mw3-recomp
  bash setup/setup.sh --iso ~/Downloads/MW3.iso
  ```

  Android only. `--game-only` rebuilds the game code after an update.
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
| Campaign and Special Ops | ✅ | Black Tuesday 118 FPS on the Thor, 60 on the Flip; Survival 118 and 60 |
| Multiplayer: System Link | ✅ | Thor and Flip in one match, 119 and 60 FPS |
| Multiplayer: Find Match, through the project's server (not Xbox LIVE) | ✅ | Free-for-all, Thor and Flip, 119 and 60 FPS |
| Bots in matches your device hosts | ✅ | 0 to 17; they join, fight and score |
| Everything unlocked, and extra camos | ✅ | chosen camos show in matches |
| Split screen (multiplayer and Special Ops) | ⚠️ | works on Windows with two controllers. Flip 2, the second controller simulated (none was at hand): a Team Deathmatch at 60 FPS, Survival for two at 48 |
| Windows PC | ⚠️ | Survival, split screen, System Link and Find Match between two copies play; campaign not tested there |
| Voice chat | ❔ | built, not tested |

Sources and methods: [docs/android.md](docs/android.md),
[docs/multiplayer.md](docs/multiplayer.md).

## Tips

- **Playing from the ISO.** The game reads its files out of the `.iso` where it
  lies, on the device or an SD card, so keep it there. **Settings → Files**
  copies the game into the app instead (about 7 GB more) and moves it back.
- **AYN Thor stuck at 60 Hz?** Turn on **Settings → Display → Smooth Display**
  in Android, or the refresh-rate tile in the quick settings. The dual-screen
  assistant can turn it off again after a restart.
- **Your own map packs (DLC).** Copy
  `Content\0000000000000000\415608CB\00000002` off your Xbox 360's hard
  disk, then **Add DLC → Choose a folder** in the app: nothing is built and
  no PC is needed. The packs' multiplayer maps are under **BONUS MAPS** at
  the top of the map list, and their Survival maps and Special Ops missions
  under **SPEC OPS MAP PACKS** in the launcher. Online, every player needs
  the map's pack. The app has no packs and fetches none:
  [docs/dlc.md](docs/dlc.md#modern-warfare-3-map-packs-survival-maps-and-special-ops-missions).
- **Custom camos.** **Settings → Multiplayer → Camo packs** adds a pack from a
  file or fetches the project's: [docs/textures.md](docs/textures.md#camouflage-packs).
- **Bots.** Tap **Bots** under Multiplayer, then **System Link → Create Game →
  Start Game**; bots only join matches your device hosts.
- **Settings** are on the app's start screen: network, multiplayer, files,
  graphics. In a game, the pause menu has **Edit controls**.
- **Online** play goes through the project's relay at `mw3.yassin.app:3074`,
  which sees your IP address like any game server
  ([docs/relay.md](docs/relay.md)). **LAN** plays on your own WiFi only.
- The first visit to a level may stutter while its graphics are compiled;
  later visits reuse what was.

## Troubleshooting

| Problem | What to do |
|---|---|
| "The game code is not on this device" | Tap **Build on this device** (step 2 above) |
| "The game code does not match this version" | After an update: tap **Build the game code** again |
| "…is not a version of the game this app supports" | The `.iso` is another game or a disc build this port was not made for ([accepted builds](tools/check_mw3_xex.py)) |
| The build stopped or failed | What was done is kept: tap **Build** again. If it fails again, the screen says where its report is; share it in an issue, after reading it (it can name files and folders) |
| Stuck at 60 Hz on the AYN Thor | See Tips: Smooth Display |
| Can't connect online, or friends don't see your game | **Settings → Network** on **Online** (restart the game after changing it); everyone on the same app version; the host must have *started* the match |
| System Link: the other device's game is not listed, or the overlay says `LAN: 0` | Both devices must be on the same Wi-Fi network, behind the same router. With two routers, a mesh's second network or a guest network they cannot hear each other: set **Settings → Network** to **Online** on both (restart the game) |
| "The disc image is not there any more" | Put the `.iso` back (SD card, moved file) or choose it again with **Install & Play** |
| Black screen or crash on a non-Qualcomm phone | Untested hardware; the app's **Log** screen says what happened |
| `setup.cmd` problems | Logs are in `out\logs\`; use a short path such as `C:\mw3`; accept the USB debugging prompt on the phone |

Bug reports are welcome with the log (the app's **Log** screen).

<details>
<summary><b>For developers</b></summary>

The port: [docs/ANDROID-ARCHITECTURE.md](docs/ANDROID-ARCHITECTURE.md),
[docs/MW3-BRINGUP.md](docs/MW3-BRINGUP.md),
[docs/MW3-MP-BRINGUP.md](docs/MW3-MP-BRINGUP.md),
[docs/runtime.md](docs/runtime.md), [docs/rendering.md](docs/rendering.md),
[docs/audio.md](docs/audio.md), [docs/multiplayer.md](docs/multiplayer.md),
[docs/building.md](docs/building.md),
[docs/ANDROID-PROGRESS.md](docs/ANDROID-PROGRESS.md),
[docs/MW3-LOCAL-RUN.md](docs/MW3-LOCAL-RUN.md). Guest addresses are in
`runtime/title.h`; recompiler settings in `config/MW3.toml` and
`config/MW3MP.toml`.

- The app and the game code are two packages, checked against each other when
  the game starts: [docs/game-library.md](docs/game-library.md). The app
  carries no game code; `tools/check_runtime_apk.py` refuses one that does.
- Asset-free tests: `cmake -S . -B build-tests -G Ninja -DMW2_TESTS_ONLY=ON && cmake --build build-tests && ctest --test-dir build-tests`
- Releasing: [docs/android-releases.md](docs/android-releases.md). Changes:
  [CHANGELOG.md](CHANGELOG.md).
- `tools/check_no_game_files.py` (also `--staged`, as a pre-commit check)
  refuses commits that carry game data. Never commit or publish anything from
  `mw3/`, `ppc_*/`, `out/` or a build folder.

</details>

## Credits

This port stands on the work of others:

- [mw2-recompiled](https://github.com/PaulCombal/mw2-recompiled) by Paul
  Combal and contributors: the runtime, the recompilation pipeline, the relay
  and the online play this grew from
- [mw3recomp](https://github.com/the-drk/mw3recomp) by DrK: the MW3 desktop
  bring-up
- mw2-android (OpenPort-Labs; its repository no longer answers) and its fork
  [flavinxs0/mw2-android](https://github.com/flavinxs0/mw2-android)
  (GuruMachanica, flavinxs0 and contributors): the Android layer
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev): the
  PowerPC recompiler
- [Xenia](https://github.com/xenia-project/xenia): the reference for the
  console's behaviour, and its FFmpeg branch's XMA decoder
- [LLVM](https://github.com/llvm/llvm-project) (clang and lld in the on-device
  build tools) and [AOSP bionic](https://android.googlesource.com/platform/bionic)'s headers
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
- Camouflage packs: [Sloth](https://forum.plutonium.pw/user/sloth)
  (SadSlothXL) for Solid Colours, Bubble Gum, Interstellar, Exclusion Zone,
  Classic Camos and the Animated Camos Mod whose look the Animate option
  follows, and [jdhruv](https://forum.plutonium.pw/user/jdhruv) for Diamond
  (threads and licences: [docs/textures.md](docs/textures.md#camouflage-packs))

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
