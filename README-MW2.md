# MW2 Recompiled: Modern Warfare 2 on Android

*Call of Duty: Modern Warfare 2* (2009) for the Xbox 360, turned into a native
Android app: the campaign, Special Ops and the multiplayer in one. The
console's PowerPC code is translated ahead of time into C++ by
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and compiled for your
phone or handheld, with a runtime that stands in for the console: its kernel,
the GPU (drawn with Vulkan), audio and controllers. It is not an emulator, and
there is no streaming.

> **You need your own copy of the game.** This repository contains no game
> data, no executables and no code made from them. You supply a disc image
> (`.iso`) of your own Xbox 360 *Modern Warfare 2* disc; the build reads it on
> your PC and everything made from it stays on your own devices. **Never upload
> or share the game code package, the extracted files or the generated code**
> (`out/`, `mw2/`, `ppc_tu0/`, `ppc_mp_tu0/`): they contain the game.

This is the same code as [mw3-recomp](https://github.com/yassinsolim/mw3-recomp),
which does this for *Modern Warfare 3* and builds for Windows as well; the two
repositories differ only in which app their releases carry.

## Status

The game is the disc's own version (1.0.557, no title update). Measured on two
handhelds with the driver the app brings (Turnip, Mesa 27.0.0), touch controls
and the app's defaults; the numbers are frames a second from the game's own
report. Other hardware is untested.

| | AYN Thor (Snapdragon 8 Gen 2, Adreno 740) | Retroid Pocket Flip 2 (Snapdragon 865, Adreno 650) |
|---|---|---|
| Frame rate cap (the app's default) | 60 | 30, without the sun's shadows |
| Installing the game files from the disc image, in the app | works | works |
| Campaign: S.S.D.D. | 60.0 | 29.5 |
| Campaign: Team Player | 57 to 58 (52 on the first visit) | 30.0 (28.5 on the first visit) |
| Campaign: Cliffhanger, start | 59.0 | 20.5 |
| Campaign: No Russian, start | 58.7 | 29.7 |
| Campaign: Takedown, start | 56.9 | 28.0 |
| Campaign: Of Their Own Accord, start | 57.8 | 29.1 |
| Campaign: The Only Easy Day... Was Yesterday, start | 54.7 | 28.2 |
| Multiplayer: Terminal | 59.9 (53 on the first visit) | 30.0 (27 on the first visit) |
| Multiplayer: Rust | 59.4 | 29.6 |
| Special Ops: The Pit, started from the menus | not run | plays |
| Split screen, two devices in one match | not tested | not tested |
| The game's own menu from one title to the other | campaign to multiplayer works | multiplayer to campaign works |
| See-through walls, garbage surfaces, vertex explosions | none seen | none seen |

What to expect:

- **The first visit to a level is slower** while its graphics pipelines are
  compiled: a lower frame rate, now and then a frame of a second or more as
  the level starts, and a surface missing for a moment. Later visits use what
  was compiled.
- **A Snapdragon 865 class device plays at 30**, not 60, and not evenly: about
  a fifth of the frames come a screen refresh late, and Cliffhanger's opening
  runs at 20. The app turns the sun's shadows off there to hold 30.
- **Qualcomm devices use the bundled Turnip driver.** With the phone's own
  driver (GRAPHICS DRIVER in the app) walls and surfaces are missing for the
  first minutes of every level, which is what earlier Android ports of this
  game were known for ([docs/MW2-ANDROID.md](docs/MW2-ANDROID.md)).
- Title update 6 is not in this app.

## What you need

- Your own Xbox 360 *Modern Warfare 2* disc, dumped to an `.iso`. The build
  checks the two executables on it and accepts the one disc build it was made
  for (1.0.557); anything else is refused rather than built wrong.
- A Windows 10 or 11 PC (64-bit) with 16 GB of memory or more and about 40 GB
  free.
- A 64-bit Android phone or handheld with Android 8 or later and Vulkan, about
  7 GB free for the game files (plus room for the 7.3 GB disc image while
  installing).

## Setup (Windows)

1. Get the code: `git clone https://github.com/yassinsolim/mw2-recomp.git C:\mw2`
   (a short path without spaces).
2. Connect the phone over USB with USB debugging on, and run

       setup\setup.cmd -Game mw2 -Iso "D:\Games\MW2.iso" -Install -PushIso

3. On the phone: open **CoD: Modern Warfare 2**, choose **INSTALL & PLAY**. It
   finds the disc image in `Download` and copies the game files once, for the
   campaign, Special Ops and the multiplayer together.

The script downloads the tools it needs into `.toolchain\` (each pinned and
checked by SHA-256; only Microsoft's C++ Build Tools are installed
system-wide, with your leave), reads the executables out of your disc image
(read-only), checks them, recompiles them and builds
`out\mw2-game.apk`: the game code (`com.mw2.recomp.game`), made from your
disc, with no screen of its own. It is yours alone: never share it.

The app itself (`com.mw2.recomp`) is downloaded from this repository's
[releases](https://github.com/yassinsolim/mw2-recomp/releases). It contains
none of the game and finds the game code you built beside it. If no release
can be downloaded, setup builds the app on your PC instead and says so; that
copy does not update itself.

Useful options: `-Check` (lists what is there and what would be downloaded,
changes nothing), `-GameOnly` (build and install the game code only),
`-Dev` (build the app here), `-Device <serial>` (which phone), `-Yes` (ask
nothing). Running setup again is safe: finished steps are skipped.

## Updates

The app looks for a newer release when it starts and from **Check for
updates**, shows what changed and installs it over itself. Your game files,
saves and game code stay as they are.

Now and then an update changes how the game is recompiled. The app then says
that the game code on the phone does not match it: on the PC, `git pull`, then
`setup\setup.cmd -Game mw2 -GameOnly` with the phone connected. That takes
minutes; game files and saves are kept
([docs/game-library.md](docs/game-library.md)).

## Multiplayer

**MULTIPLAYER → SYSTEM LINK** in the game: one player creates a game, the
others find it in the list. The app's Settings → Network chooses how players
reach each other: **Online** (through the project's relay), **LAN** (your own
network only) or off. Everyone needs this same version of the game; it does
not play with consoles on Xbox LIVE.

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
- [mw3-recomp](https://github.com/yassinsolim/mw3-recomp), this project: the
  renderer and Android work since, the app that updates itself and the game
  code as a package of its own

The full list and the licences: [README.md](README.md#credits),
[THIRD_PARTY.md](THIRD_PARTY.md).

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
