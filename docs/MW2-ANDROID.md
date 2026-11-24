# Modern Warfare 2 on Android

This repository grew out of the Modern Warfare 2 port and still builds it, the
way it builds Modern Warfare 3: one app with the campaign, Special Ops and the
multiplayer (`com.mw2.recomp`), published without the game's code and updating
itself, and the recompiled code as a package of its own
(`com.mw2.recomp.game`) that each player builds from their disc
([game-library.md](game-library.md), [android-releases.md](android-releases.md)).
The game is the disc's own version (1.0.557, "tu0"). It shares the runtime
with Modern Warfare 3, and so every renderer fix made for it since. The public
repository for it is `yassinsolim/mw2-recomp`
([its front page](https://github.com/yassinsolim/mw2-recomp#readme) is README-MW2.md here).

## Status

Measured on two handhelds with the bundled Turnip driver (Mesa 27.0.0,
v27.0.0-R2), touch controls, no multisampling, 1280x720, the app's defaults.
Frame rates are the log's own (`MW2_REPORT=1`): over a recorded flight
(`tools/routes/`) for the first two levels, a recorded walk for Terminal, and
the first minute of play for the rest, which includes the level's first
pipelines being compiled.

| | AYN Thor (Snapdragon 8 Gen 2, Adreno 740), cap 60 | Retroid Pocket Flip 2 (Snapdragon 865, Adreno 650), cap 30, no sun shadows |
|---|---|---|
| Game files installed from the disc image in the app | works | works |
| Campaign, S.S.D.D. | 60.0 | 29.5 |
| Campaign, Team Player | 56.6 to 58.4 (51.6 on the first visit) | 30.0 (28.5 on the first visit) |
| Campaign, Cliffhanger (start) | 59.0 | 20.5 |
| Campaign, No Russian (start) | 58.7 | 29.7 |
| Campaign, Takedown (start) | 56.9 | 28.0 |
| Campaign, Of Their Own Accord (start) | 57.8 | 29.1 |
| Campaign, The Only Easy Day... Was Yesterday (start) | 54.7 | 28.2 |
| Multiplayer, Terminal | 59.9 (53.2 on the first visit) | 30.0 (27.0 on the first visit) |
| Multiplayer, Rust | 59.4 | 29.6 |
| System Link game hosted from the menus (Rust, Terminal) | plays | not run |
| The game's menu from one title to the other | campaign to multiplayer: restarts into it | multiplayer to campaign: restarts into it |
| Special Ops, The Pit, started from the menus | not run | plays, at the cap |
| Graphics | no see-through walls, garbage surfaces or vertex explosions in any of the above | the same |

The Flip 2's column is the earlier build's, with the cap at 30: about a fifth
of its frames came a display blank late (17 to 23% took 34 to 50 ms), and
Cliffhanger's opening on the ice wall ran at 20. An Adreno 6xx now starts
Modern Warfare 2 with the cap at 60, without the sun's shadows and with an
832x480 scene under the 1280x720 frame
([android.md](android.md#adreno-6xx-snapdragon-865-class)). With those
defaults the Flip 2 ran S.S.D.D.'s flight for five minutes at 58.1 frames a
second (70% of the frames on time, 99.9% within two blanks), Team Player's
at 48.9 (42%), Terminal's walk for five minutes at 59.7 (95%) and Rust for
three at 59.2 (95%); before, with the cap at 60, S.S.D.D. ran at 43. The
levels after the first two were not measured again.

Not tested: split screen, two devices in one match (the two handhelds here
were on different networks, and the Flip 2's did not reach the relay), title
update 6, phones without an Adreno.

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

So the app carries the Turnip driver Modern Warfare 3's app carries and uses
it by default on a Qualcomm phone (`DriverStore.installBundledDefault`;
GRAPHICS DRIVER in the launcher still chooses the phone's own or an imported
one). On an Adreno 6xx Turnip is started without image compression, which
resets the device there. Nothing else in the shared renderer had to change
for Modern Warfare 2.

## Building (Windows)

    setup\setup.cmd -Game mw2 -Iso "D:\Games\MW2.iso"

does it all, as it does for Modern Warfare 3 ([README](../README.md)): it
reads `default.xex` and `default_mp.xex` out of the image into `mw2\tu0\`,
checks their SHA-256 against the one disc the port supports, recompiles them
into `ppc_tu0\` and `ppc_mp_tu0\` (`config/MW2.tu0.toml`,
`config/MW2MP.tu0.toml`), builds the game code (`libmw2game.so`,
`libmw2mpgame.so`) into `out\mw2-game.apk`, and downloads the published app
from `yassinsolim/mw2-recomp`. When there is no release to download it builds
the app here instead (`out\mw2-debug.apk`) and says so: that one is signed
with the PC's own key and does not update itself. `-GameOnly` builds the game
code alone, `-Dev` the app as well, `-Suffix .dev` puts both beside the
published ones. On a phone that still has the two apps of old
(`cod.modern.warfare2.sp`, `.mp`), setup copies their saves into the app.

On Linux, `VERSION=tu0 RECOMPILE_ONLY=1 ./build.sh` for each `TITLE`, then
CMake with `-DMW2_SPLIT=game` and Gradle (`:game:assembleDebug -Pgame=mw2`,
`:app:assembleMw2Tu0Debug`; [building.md](building.md), [android.md](android.md)).

### Title update 6, and an app for each title

The game as title update 6 makes it is not in the one app: its executables
are made on the device from the disc's and the update's package, and nothing
here records what a library of theirs would have to be built against. It
builds as it always did, an app for each title with the recompiled code
inside (`MW2_SPLIT=OFF`): the `campaign` and `multiplayer` flavours
(`assembleCampaignTu6Release`, `assembleMultiplayerTu6Release`;
`cod.modern.warfare2.sp.tu6`, `.mp.tu6`), and for the disc's version
`assembleCampaignTu0Debug` and `assembleMultiplayerTu0Debug`. Title update 6
needs the update's package and was not built here.

## Installing and playing

Copy the disc image to the phone's `Download` folder (`setup -PushIso` does),
open the app and choose INSTALL & PLAY: the one copy of the game files serves
the campaign, Special Ops and the multiplayer. The launcher starts either,
and the game's own menu entries between the two restart the run with the
other.

A debug build reads more switches from
`/sdcard/Android/data/com.mw2.recomp/files/options.txt`, one `MW2_NAME=value`
a line ([switches.md](switches.md)): `MW2_REPORT=1` ends the log with the
frame rate, and `MW2_CONSOLE`, `MW2_FLY_PATH` and `MW2_WALK_PATH` play a level
without hands, which is how the numbers above were taken.

## Known issues

- On the phone's own Adreno driver (chosen in GRAPHICS DRIVER, or a build
  without the bundled Turnip): the missing surfaces and lower frame rate
  described above. SETTINGS can compile every shader before play instead.
- The first visit to a level on Turnip still compiles its pipelines: a few
  surfaces can be missing or black for a moment, the frame rate dips (the
  first-visit numbers above) and a frame can take seconds while a level
  starts.
- A Snapdragon 865 does not hold 60 everywhere: where a level has the most
  to draw it waits for the GPU's vertex work, S.S.D.D.'s opening at 50 to 55
  frames a second and the first half of Team Player's flight at 35 to 44,
  and the sun's shadows are off there (above).
- The Flip 2's earlier Team Player numbers were taken with part of the
  level undrawn: a frame's vertex copies did not fit its share of the arena,
  and the sky and the far buildings went missing. It is drawn whole now
  ([rendering.md](rendering.md#vertex-data-kept-without-the-shadow)). The sky
  that was gone near the flight's end on the Thor, put down then to the
  flight leaving the level, looks like the same thing; it was not looked at
  again on the Thor here.
- The Thor's HUD read 70 to 80 °C for the processor in these runs, each of
  two minutes or less; longer play was not measured.
- A scripted pad (`MW2_INPUT_SCRIPT`) does not reach the first controller on
  Android, where the touch overlay answers for it; the autopilot does.
- The disc version has none of the co-op network hardening the update's
  block of `runtime/title.h` has: its addresses were never derived for the
  disc. `MW2_RENDER` was tried on the disc's campaign at 960x600 and not in
  its multiplayer, which takes `MW2_SCENE` through the same hook.
