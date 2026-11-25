# AW Recompiled: Call of Duty: Advanced Warfare on Android

A native Android port of *Call of Duty: Advanced Warfare*, recompiled from
**your own Xbox 360 discs**: the campaign, multiplayer and Exo Survival in one
app. It is not an emulator and nothing is streamed: the console's code is
translated ahead of time by [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)
and compiled for your device, with a small runtime standing in for the console
(Vulkan graphics, audio, controllers). It is the newest of the four ports and
the least played: **read [Known limits](#known-limits) before you start**.

> **Unofficial fan project.** Not affiliated with or endorsed by Activision or
> Microsoft. This repository contains no game data, no executables and no code
> made from them. You bring your own disc images; everything built from them
> stays on your own devices. **Never upload or share the game code package, the
> extracted files or the generated code**: they contain the game.

Other games: [Modern Warfare 3](https://github.com/yassinsolim/mw3-recomp)
(also builds for Windows) · [Modern Warfare 2](https://github.com/yassinsolim/mw2-recomp) ·
[Call of Duty 4](https://github.com/yassinsolim/cod4-recomp)

## What you need

- **Both of your own *Call of Duty: Advanced Warfare* Xbox 360 discs, each as
  an `.iso`** (8.7 GB each): the **game disc** (Disc 1) and the **install
  disc** (Disc 2). The game does not start without the install disc's
  content, just as the console asks you to install it first. Normal PC disc
  drives can't read Xbox 360 discs, so the dumps have to come from a drive
  that can.
- **The version:** the USA/Europe release of 2014, as it is on the discs
  (no title update). Any other game disc is refused rather than built wrong.
- **An Android 10 or newer device with a Snapdragon 865 or better**, 8 GB of
  RAM recommended. Tested: AYN Thor (Snapdragon 8 Gen 2) and Retroid Pocket
  Flip 2 (Snapdragon 865, the same chip as the Retroid Pocket 5). Mali GPUs
  are untested with this game.
- **Both `.iso` files stay on the device or its SD card** (17.5 GB): by
  default the game plays straight from them and copies nothing but its two
  executables, so the device's own storage needs only about 1 GB while the
  game code is built. Or the app can copy the game out of the two images
  (7.3 GB from the game disc, 4 GB from the install disc, about 12 GB on the
  device's own storage), after which the images are no longer needed.

## Quick start (no PC)

1. Install the APK from [Releases](https://github.com/yassinsolim/aw-recomp/releases/latest)
   and open it.
2. Tap **Build on this device**. With storage access allowed, the app finds
   your **game disc's** `.iso` by itself wherever it lies on the device or
   the SD card; otherwise pick it with the Files app, or browse to it and
   tap the `.iso` (or **Use this folder** in the folder it is in). The image
   is only read, and you never have to extract anything from it. The app
   downloads its build tools once (43 MB); Android asks twice to allow
   installing them.
3. Tap **Build** and wait about **13 minutes** (measured on the Retroid
   Pocket Flip 2; this game is the largest of the four). You can leave the
   app or lock the screen; it carries on. Keep the device charging: the build
   uses every core.
4. Tap **Install & Play**. With both images in one folder the app takes the
   install disc along with the game disc and the game starts within seconds,
   **playing from the two images where they are**. If it asks for the second
   disc, choose the install disc's `.iso` (in the Files app you can select
   both at once) and tap **Install**. To copy the game into the app instead,
   open **Install the game files**, choose **Copy the game files into the
   app** and tap **Install**; the app remembers which of the two you chose.
5. Play: **Campaign**, **Multiplayer**, or **Exo Survival** from the game's
   own main menu, with the touch overlay or a Bluetooth/USB controller. The
   first start asks you to move the brightness slider and then the screen
   margins before it takes **A**: that is the game, not a fault.

The app updates itself. When an update needs the game code made again it
says so; tap **Build the game code** (no disc image is asked for).

<details>
<summary><b>Other ways to install</b></summary>

- **Windows PC**: get the code (`git clone https://github.com/yassinsolim/aw-recomp.git C:\aw`,
  a short path without spaces), connect the phone over USB with USB debugging
  on, and run

  ```
  setup\setup.cmd -Game aw -Iso "D:\Games\AW-game-disc.iso" -InstallIso "D:\Games\AW-install-disc.iso" -Install -PushIso
  ```

  It downloads what it needs into `.toolchain\` (only Microsoft's C++ Build
  Tools are installed system-wide, with your leave), builds `out\aw-game.apk`,
  installs it with the published app, copies the install disc's content to
  the phone and the game disc's image to its Download folder. `-Build pc` or
  `-Build both` makes the Windows game as well, in `out\pc-aw`. After an app
  update that needs new game code: `git pull`, then
  `setup\setup.cmd -Game aw -GameOnly`. `-Check` lists what it would do and
  changes nothing.
- **Linux PC** (tested on Ubuntu 24.04, WSL2 included), with the phone
  connected over USB:

  ```sh
  git clone https://github.com/yassinsolim/aw-recomp.git && cd aw-recomp
  bash setup/setup.sh --game aw --iso ~/Downloads/AW-game-disc.iso --install-iso ~/Downloads/AW-install-disc.iso --install --push-iso
  ```

  `--game-only` rebuilds the game code after an update.
- **GitHub builds it for you**: not available for this game.

How the on-device build works and what was measured:
[docs/ondevice-build.md](docs/ondevice-build.md). The other ways, step by
step: [docs/BUILD-WITHOUT-PC.md](docs/BUILD-WITHOUT-PC.md).

</details>

## What works

Measured on 24 November 2026 on an AYN Thor (Snapdragon 8 Gen 2,
120 Hz) and a Retroid Pocket Flip 2 (Snapdragon 865, 60 Hz) with the app's
defaults. Other hardware is untested.

| | | Measured |
|---|---|---|
| Campaign: New Game, Induction (Seoul) to its rooftop | ✅ | 60 FPS on the Flip 2, 118 to 119 on the Thor |
| Campaign: a heavy level (Fusion's opening) | ✅ | 60 FPS on the Flip 2, 115 on the Thor |
| Checkpoints and Resume Game | ✅ | Flip 2: stopped the app at a checkpoint, Resume Game went on from it |
| Multiplayer: System Link with bots (up to 11) | ✅ | Team Deathmatch with 6 bots on Retreat: 60 FPS on the Flip 2, 119 on the Thor |
| Multiplayer: two devices in one match | ✅ | Thor and Flip 2 over the project's server, each hosting in turn: 119 and 60 FPS |
| Multiplayer: Find Match, through the project's server (not Xbox LIVE) | ✅ | Thor and Flip 2 met in Team Deathmatch, 119 and 60 FPS |
| Exo Survival | ⚠️ | starts from the campaign's menu and plays, alone or two together; see Known limits |
| Setting up with no PC, from a fresh install of v0.1.0 | ✅ | Flip 2: the game code built in 13 min 8 s, each disc copied in about 40 s, 18 minutes from opening the app to the game's title screen. Not run on the Thor |
| Playing from both images on an SD card, nothing copied | ✅ | Flip 2: found on the card by itself, the game starts 2 s after **Install & Play**; Seoul loads in 14 s and a match on Retreat in under 8 s, the same as from copied files |
| Your own map packs (with a PC: `setup -Dlc`) | ⚠️ | Thor: Kremlin with 6 bots and Atlas Gorge against a PC through the project's server, 119 FPS. Flip 2: Atlas Gorge against a PC, 60 FPS |
| Exo Zombies (with your map packs) | ⚠️ | Thor: Outbreak starts and plays at 119 FPS; rounds 2 to 7 only with a test build that kept the player alive. Not run on the Flip 2 |
| Combat Readiness Program, split screen | ❔ | not tested on a device |

## Known limits

This is a first release. What has **not** been shown:

- **No level has been played from start to finish by a person.** Induction
  (Seoul) has been played through its opening to the rooftop and beyond, its
  ending was reached from a developer start point, and the next level (Atlas)
  starts from it. The other fourteen levels load from the same code; most
  were never opened. Expect a level somewhere that stops you.
- **Exo Survival's later waves have only been proven with test tools.** By
  normal play, on a device, nobody has survived round 1 yet; rounds 2 to 5
  were reached only with a test build that kept the player alive.
- **The campaign saves at every checkpoint**, where the console wrote the save
  only when you chose Save and Quit. This is on purpose (Android may close the
  app at any time), but it is not how the console behaved.
- **The frame rate is not locked.** The Flip 2 holds 60 in what was measured;
  the Thor holds 115 to 119, with Fusion at 115. The first visit to a level
  stutters while its graphics are compiled; later visits reuse what was.
- **Sun shadows are off above 60 FPS** (a setting turns them on, at a cost).
- **Map packs and Exo Zombies need a PC and your own files.** The app plays
  the disc's version. With your own map packs, Compatibility Packs and title
  update copied off your console, `setup\setup.cmd -Game aw -Dlc <folder>`
  builds the updated multiplayer and the app gets **Add DLC** and **Exo
  Zombies** entries ([docs/dlc.md](docs/dlc.md#advanced-warfare-map-packs-and-exo-zombies)).
  Exo Zombies was seen on the AYN Thor only, its later rounds only with test tools;
  the campaign stays the disc's.
- **Online play is the project's own server**, not Xbox LIVE: no ranks carry
  over, the Friends list is empty, and everyone needs the same app version.
- **Only two devices have run it.** Anything that is not a Snapdragon with an
  Adreno GPU may not start at all.

What has been checked and what is open, in detail:
[docs/AW-BRINGUP.md](docs/AW-BRINGUP.md).

## Tips

- **Bots.** Tap **Bots** under Multiplayer on the app's start screen, then
  host a **System Link** game; bots only join matches your device hosts.
- **Exo Survival** is on the game's own main menu (Campaign, Multiplayer, Exo
  Survival): the app switches over by itself.
- **Everything is unlocked** in Create a Class (Settings → Multiplayer →
  "Unlock all items", on by default).
- **Settings** are on the app's start screen: network, multiplayer, files,
  graphics. In a game, the pause menu has **Edit controls**.
- **Online** play goes through the project's relay at `mw3.yassin.app:3074`,
  which sees your IP address like any game server
  ([docs/relay.md](docs/relay.md)). **LAN** plays on your own WiFi only.
- **AYN Thor stuck at 60 Hz?** Turn on **Settings → Display → Smooth Display**
  in Android, or the refresh-rate tile in the quick settings.
- **Free the space, or keep it.** Played from the images, the game needs
  both `.iso` files where they are (an SD card is fine) and almost nothing
  on the device's own storage. Copied into the app, it needs about 12 GB
  there and the `.iso` files can go. **Settings → Change where the game is
  read from** switches between the two and offers to remove the copies.

## Troubleshooting

| Problem | What to do |
|---|---|
| "The game code is not on this device" | Tap **Build on this device** (step 2 above) |
| The build screen already says "found" | With storage access allowed, a game disc image anywhere on the device or its SD card is found by itself; nothing to pick |
| "…holds neither a disc image (.iso) of this game nor a disc's extracted files" | The folder you chose has no image of this game in it: open the folder the `.iso` is in and tap the `.iso` itself, or use **Choose with the Files app** |
| "Play from the disc image" is greyed out | What is chosen is a folder of extracted game files, which can only be copied; the screen says so. Choose the `.iso` itself to play without copying |
| "The disc image the game is played from is not there any more" | The SD card is out, or an `.iso` was moved or deleted: put it back, or choose it again with **Install the game files** |
| "The game code does not match this version" | After an update: tap **Build the game code** again |
| "…is not a version of the game this app supports" | The `.iso` is another game, the install disc where the game disc is wanted, or not the USA/Europe disc this port was made for |
| The app keeps asking for the second disc | The game needs both: choose the other disc's `.iso` (the install disc after the game disc) |
| Nothing happens on the Brightness screen | Move the slider left or right first, then press **A**; the same on the next screen |
| Not enough space | Play from the images (the default), which copies nothing; copying needs about 12 GB on the device's own storage |
| The build stopped or failed | What was done is kept: tap **Build** again. If it fails again, the screen says where its report is; share it in an issue, after reading it (it can name files and folders) |
| Can't connect online, or friends don't see your game | **Settings → Network** on **Online** (restart the game after changing it); everyone on the same app version; the host must have *started* the match |
| Black screen or crash on a non-Qualcomm phone | Untested hardware; the app's **Log** screen says what happened |
| `setup.cmd` problems | Logs are in `out\logs\`; use a short path such as `C:\aw`; accept the USB debugging prompt on the phone |

Bug reports are welcome with the log (the app's **Log** screen), the device
and the level.

<details>
<summary><b>For developers</b></summary>

This is the same code as [mw3-recomp](https://github.com/yassinsolim/mw3-recomp);
the repositories differ only in which app their releases carry.

- How it works: [docs/AW-BRINGUP.md](docs/AW-BRINGUP.md),
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
  `aw/`, `ppc_*/`, `out/` or a build folder.

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
sponsored by Activision, Sledgehammer Games, High Moon Studios or Microsoft.
*Call of Duty* and *Call of Duty: Advanced Warfare* are trademarks of
Activision Publishing, Inc.; *Xbox 360* and *Xbox LIVE* are trademarks of
Microsoft. No game content is distributed here: you need your own legally
obtained copy of the game, and what you build from it is for your own use
only.
