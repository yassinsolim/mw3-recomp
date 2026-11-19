# Modern Warfare 2 on Android

This repository grew out of the Modern Warfare 2 port and still builds it: the
campaign (with Special Ops) and the multiplayer, as two apps
(`cod.modern.warfare2.sp`, `cod.modern.warfare2.mp`), from the disc's own
executables (version 1.0.557, "tu0"). They share the runtime with Modern
Warfare 3, and so every renderer fix made for it since.

## Status

Tested on an AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13) with the
bundled Turnip driver (Mesa 27.0.0, v27.0.0-R2), touch controls, 60 FPS cap,
no multisampling, 1280x720 (the app's defaults). Frame rates are the log's own
(`MW2_REPORT=1`), over a recorded flight or walk (`tools/routes/`).

| | result |
|---|---|
| Build (XenonRecomp, Gradle, Windows) | both apps build; 113 and 117 recompiled files, 100 and 121 jump tables, no recompiler errors |
| Install from the disc image in the app | works (6.7 GB, both apps) |
| Campaign, S.S.D.D. | 59.9 frames a second; 91% of frames within 17.5 ms |
| Campaign, Team Player | 56.6 frames a second (48.6 on the first visit, while its pipelines compile) |
| Campaign, Cliffhanger, Takedown, Of Their Own Accord, The Only Easy Day | load and draw correctly at their starts, about 60 |
| Multiplayer, System Link match on Terminal | 59.5 frames a second |
| Multiplayer, System Link match on Rust | 59.9 frames a second |
| Graphics | no vertex explosions, black world, missing walls or wrong textures found on Turnip in any of the above |

Not tested: split screen, two devices in one match, title update 6 (needs the
update's package), Special Ops, other phones.

## What the old port's graphics problems were

The earlier Android port drew with the phone's own Vulkan driver. On the
Thor's (Qualcomm's, 2023) the same build shows them:

- **Missing geometry, see-through walls, white or garbage surfaces** for the
  first minutes of a level, on every visit. A pipeline is compiled on another
  thread while the frame goes on without its draws (`MW2_ASYNC_PIPELINES`),
  and Qualcomm's driver takes up to half a second for each: 534 pipelines
  took 90 s in S.S.D.D. on its second visit, 417 took 54 s on Terminal.
  Turnip took 16 s for Terminal's 432 on the first visit and 52 s for Team
  Player's 1,134, and 0.2 to 1.5 s for a level on a later visit.
- **A lower frame rate**: S.S.D.D. ran at 36 to 49 frames a second against
  60. On Qualcomm's driver the runtime also decodes block-compressed textures
  in software, since sampling them in hardware flickers black there.

So the MW2 apps now carry the Turnip driver Modern Warfare 3's app carries and
use it by default on a Qualcomm phone (`DriverStore.installBundledDefault`;
GRAPHICS DRIVER in the launcher still chooses the phone's own or an imported
one). Nothing in the shared renderer had to change for MW2.

## Building (Windows)

    setup\setup.cmd -Game mw2 -Iso "D:\Games\MW2.iso"

does it all, as it does for Modern Warfare 3 ([README](../README.md)): it
reads `default.xex` and `default_mp.xex` out of the image into `mw2\tu0\`,
checks their SHA-256 against the one disc the port supports, recompiles them
into `ppc_tu0\` and `ppc_mp_tu0\` (`config/MW2.tu0.toml`,
`config/MW2MP.tu0.toml`) and builds `out\mw2-sp-debug.apk` and
`out\mw2-mp-debug.apk` (`assembleCampaignTu0Debug`,
`assembleMultiplayerTu0Debug`). `-Title sp` or `-Title mp` builds one app.
On Linux, `VERSION=tu0 RECOMPILE_ONLY=1 ./build.sh` for each `TITLE`, then
Gradle ([building.md](building.md), [android.md](android.md)).

Title update 6 (the `tu6` flavours) needs the update's package and was not
built here.

## Installing and playing

Install the APKs, copy the disc image to the phone's `Download` folder, open
an app and choose INSTALL & PLAY; each app keeps its own copy of the game
files. With both installed, each launcher offers to start the other.

A debug build reads more switches from
`/sdcard/Android/data/<package>/files/options.txt`, one `MW2_NAME=value` a
line ([switches.md](switches.md)): `MW2_REPORT=1` ends the log with the frame
rate, and `MW2_CONSOLE`, `MW2_FLY_PATH` and `MW2_WALK_PATH` play a level
without hands, which is how the numbers above were taken.

## Known issues

- On the phone's own Adreno driver (chosen in GRAPHICS DRIVER, or a build
  without the bundled Turnip): the missing surfaces and lower frame rate
  described above. SETTINGS can compile every shader before play instead.
- The first visit to a level on Turnip still compiles its pipelines: a few
  surfaces can be missing or black for a moment (seen once, over the tents of
  S.S.D.D.), and the frame rate dips (Team Player, 48.6 instead of 56.6).
- The Thor's HUD read 80 to 88 °C for the processor in these runs, each of
  two minutes or less; longer play was not measured.
- A scripted pad (`MW2_INPUT_SCRIPT`) does not reach the first controller on
  Android, where the touch overlay answers for it; the autopilot does.
- The disc version's campaign has no `MW2_RENDER` sizes and none of the co-op
  network hardening the update's block of `runtime/title.h` has: their
  addresses were never derived for the disc.
