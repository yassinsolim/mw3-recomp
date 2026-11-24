# CoD4 Recompiled: Call of Duty 4: Modern Warfare on Android

*Call of Duty 4: Modern Warfare* (2007) for the Xbox 360, turned into a native
Android app: the campaign and the multiplayer in one. The console's PowerPC
code is translated ahead of time into C++ by
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and compiled for your
phone or handheld, with a runtime that stands in for the console: its kernel,
the GPU (drawn with Vulkan), audio and controllers. It is not an emulator, and
there is no streaming.

> **You need your own copy of the game.** This repository contains no game
> data, no executables and no code made from them. You supply a disc image
> (`.iso`) of your own Xbox 360 *Call of Duty 4: Modern Warfare* disc; the
> build reads it on your PC and everything made from it stays on your own
> devices. **Never upload or share the game code package, the extracted files
> or the generated code** (`out/`, `cod4/`, `ppc_cod4/`, `ppc_cod4_mp/`): they
> contain the game.

This is the same code as [mw3-recomp](https://github.com/yassinsolim/mw3-recomp)
(*Modern Warfare 3*) and [mw2-recomp](https://github.com/yassinsolim/mw2-recomp)
(*Modern Warfare 2*); the three repositories differ only in which app their
releases carry. Call of Duty 4 is the newest of the three and the least
played: expect rough edges.

## Status

The game is the disc's own version (no title update). Measured on two
handhelds with the driver the app brings (Turnip, Mesa 27.0.0), touch controls
and the app's defaults, on 23 November 2026; the numbers are frames a second
from the game's own report over 150 to 190 seconds of play. Other hardware is
untested.

| | AYN Thor (Snapdragon 8 Gen 2, Adreno 740, 120 Hz) | Retroid Pocket Flip 2 (Snapdragon 865, Adreno 650, 60 Hz) |
|---|---|---|
| Frame rate cap (the app's default) | 120, the screen's | 60, the screen's |
| Installing the game files from the disc image, in the app | works | works |
| Opening movies, title screen, main menu | 119 | 60 |
| Campaign: F.N.G. | 119.0 | 60.0 |
| Campaign: Crew Expendable | 119.0 | 59.9 |
| Campaign: Charlie Don't Surf | 117.4 (117.7 on the first visit) | 59.5 (59.1 on the first visit) |
| Multiplayer: Crash, a System Link match the device hosts | 119.1 | 60.0 |
| Multiplayer: the two devices in one System Link match (Backlot), through the project's relay | both spawn and play, ten minutes to the time limit | both spawn and play |
| Multiplayer: Xbox LIVE Find Match (Team Deathmatch), the two devices through the project's relay | 119, six and a half minutes in the match | 60 |
| Multiplayer: Xbox LIVE Find Match (Free-for-all), and a private match joined from the pause menu, the two devices through the project's relay | 119 | 60 |
| Checkpoints: saved, and loaded by Resume Game | works on Windows; not checked on Android | not checked |
| Bots in the matches the device hosts (the launcher's **Bots** entry) | not run | join, fight and score: six on Crash for a ten-minute match, 60 |
| Split screen; invitations on Xbox LIVE | not tested | not tested |

What to expect:

- **Charlie Don't Surf stutters where the helicopters land**: 30 to 70 late
  frames in that half minute on both devices. The rest of the level, and the
  other levels measured, hold the screen's rate.
- **The first visit to a level compiles its graphics pipelines**: now and
  then a late frame as the level starts. Later visits use what was compiled.
- **Only three campaign levels and two multiplayer maps have been played**
  for more than a few minutes. The others load from the same code and have
  not been checked one by one.
- **Qualcomm devices use the bundled Turnip driver.** Mali and other graphics
  chips are untested with this game.
- The sun's shadows are off above 60 frames a second, as in the *Modern
  Warfare 3* app: with them the AYN Thor drops to 114 to 115 with hundreds of
  late frames.

What has been checked and how, and what is still open:
[docs/COD4-BRINGUP.md](docs/COD4-BRINGUP.md).

## What you need

- Your own Xbox 360 *Call of Duty 4: Modern Warfare* disc, dumped to an
  `.iso` (7.8 GB). The build checks the two executables on it and accepts the
  one disc build it was made for (USA/Europe, 2007); anything else is refused
  rather than built wrong.
- A Windows 10 or 11 PC (64-bit) with 16 GB of memory or more and about 40 GB
  free. With a Linux PC, see
  [Setup without a Windows PC](#setup-without-a-windows-pc).
- A 64-bit Android phone or handheld with Android 8 or later and Vulkan, about
  7 GB free for the game files (plus room for the 7.8 GB disc image while
  installing).

## Setup (Windows)

1. Get the code: `git clone https://github.com/yassinsolim/cod4-recomp.git C:\cod4`
   (a short path without spaces).
2. Connect the phone over USB with USB debugging on, and run

       setup\setup.cmd -Game cod4 -Iso "D:\Games\CoD4.iso" -Install -PushIso

3. On the phone: open **CoD 4: Modern Warfare**, choose **INSTALL & PLAY**. It
   finds the disc image in `Download` and copies the game files once, for the
   campaign and the multiplayer together.

The script downloads the tools it needs into `.toolchain\` (each pinned and
checked by SHA-256; only Microsoft's C++ Build Tools are installed
system-wide, with your leave), reads the executables out of your disc image
(read-only), checks them, recompiles them and builds
`out\cod4-game.apk`: the game code (`com.cod4.recomp.game`), made from your
disc, with no screen of its own. It is yours alone: never share it.

The app itself (`com.cod4.recomp`) is downloaded from this repository's
[releases](https://github.com/yassinsolim/cod4-recomp/releases). It contains
none of the game and finds the game code you built beside it. If no release
can be downloaded, setup builds the app on your PC instead and says so; that
copy does not update itself.

Useful options: `-Check` (lists what is there and what would be downloaded,
changes nothing), `-GameOnly` (build and install the game code only),
`-Dev` (build the app here), `-Device <serial>` (which phone), `-Yes` (ask
nothing), `-Build pc` or `-Build both` (the Windows game as well, in
`out\pc-cod4`). Running setup again is safe: finished steps are skipped.

## Setup without a Windows PC

- **A Linux PC** (tested on Ubuntu 24.04, WSL2 included), one command, with
  the phone connected over USB:

      git clone https://github.com/yassinsolim/cod4-recomp.git && cd cod4-recomp
      bash setup/setup.sh --game cod4 --iso ~/Downloads/CoD4.iso --install --push-iso

  It fetches its tools into `.toolchain/`, builds `out/cod4-game.apk`,
  downloads the published app and installs both. It runs as many compilers
  as the free memory holds, so an old PC is slower, not stuck;
  `--game-only` rebuilds the game code after an update.

Step by step, with what an old PC should expect:
[docs/BUILD-WITHOUT-PC.md](docs/BUILD-WITHOUT-PC.md).

## Updates

The app looks for a newer release when it starts and from **Check for
updates**, shows what changed and installs it over itself. Your game files,
saves and game code stay as they are.

Now and then an update changes how the game is recompiled. The app then says
that the game code on the phone does not match it: on the PC, `git pull`, then
`setup\setup.cmd -Game cod4 -GameOnly` with the phone connected. That takes
minutes; game files and saves are kept
([docs/game-library.md](docs/game-library.md)).

## Multiplayer

The app's Settings → Network chooses how players reach each other: **Online**
(through the project's relay, the default), **LAN** (your own network only)
or off.

- **MULTIPLAYER → XBOX LIVE → FIND MATCH**, with Online: the relay puts the
  players who search together. Team Deathmatch and Free-for-all on the
  disc's sixteen maps.
- **MULTIPLAYER → SYSTEM LINK**, with Online or LAN: one player creates a
  game, the others find it in the list.

Everyone needs this same version of the game; it does not play with consoles
on Xbox LIVE.

**Bots.** On the start screen, tap **Bots** (under **Multiplayer**) and
choose how many and how good; the entry then reads `BOTS · 6 · REGULAR`.
Then start Multiplayer and host a System Link game: the bots join the
matches *your* device hosts, pick a team and a class, fight and score, and
leave to make room when a player joins. The app's pause menu changes their
number in a match.

## Credits

This port stands on the work of others:

- [mw2-recompiled](https://github.com/PaulCombal/mw2-recompiled) by Paul
  Combal and contributors: the runtime, the recompilation pipeline, the relay
  and the online play all of this grew from
- [mw2-android](https://github.com/OpenPort-Labs/mw2-android), the original
  Android port by Player124413 and OpenPort-Labs, and its fork
  [flavinxs0/mw2-android](https://github.com/flavinxs0/mw2-android) by
  flavinxs0 and contributors: the Android layer
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev): the
  PowerPC recompiler
- [Xenia](https://github.com/xenia-project/xenia): the reference for the
  console's behaviour, and its FFmpeg branch's XMA decoder
- [libadrenotools](https://github.com/bylaws/libadrenotools) and
  [Mesa Turnip](https://gitlab.freedesktop.org/mesa/mesa) (builds by
  [StevenMXZ](https://github.com/StevenMXZ/Adreno-Tools-Drivers)): the
  graphics driver on Adreno
- [mw3recomp](https://github.com/the-drk/mw3recomp) by DrK: the *Modern
  Warfare 3* desktop bring-up this runtime also carries
- [mw3-recomp](https://github.com/yassinsolim/mw3-recomp), this project: the
  renderer and Android work since, the app that updates itself and the game
  code as a package of its own

The full list and the licences: [THIRD_PARTY.md](THIRD_PARTY.md).

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
