# Visual QA: recording a run and checking every frame

`tools/visualqa/visualqa.py` plays a scenario on a phone (or a Windows
build), records the screen while it does, reads every frame of the recording
and the run's own log, and prints `PASS` or `FAIL` with the faults it found
and a picture of each. It exists because flicker, missing ground and frozen
pictures were being found by players: a run that "reached the level at 60
frames a second" by its log says nothing of what was on the screen.

Every change that can touch the picture or the pacing is run through it
before it is merged, and the whole suite is a release gate
([android-releases.md](android-releases.md#each-release)).

    pip install --user numpy opencv-python

    # every standard scenario on both phones
    python tools/visualqa/visualqa.py --out <folder> suite --apks <folder of test apps> --lock-dir <device-locks>

    # one scenario on one phone
    python tools/visualqa/visualqa.py --out <folder> run mw3-bots --device thor --apks <folder> --lock-dir <device-locks>

    # a Windows build, from the frames it writes itself
    python tools/visualqa/visualqa.py --out <folder> windows mw2-campaign -- build-win\mw2.exe mw2\default.pe mw2\game

    # a recording somebody sent, with or without its log
    python tools/visualqa/visualqa.py --out <folder> analyse screen.mp4 --mask touch [--log game.log]

A recording made by hand has menus and fights in it. With a log that has a
line a second (`MW2_REPORT=1`, `MW2_REPORT_SECONDS=1`, `MW2_STUTTERS=1` in
the run) `analyse` knows which seconds were play; without one it only
remarks on patches that differ for a single frame, unless `--play` says the
recording is calm play throughout or `--menu` that it is menus, and a log
that ends without its closing report is a run somebody closed, not a
failure. Whole-frame flicker, blocks and black frames are judged either way.

The exit status is 1 when a run failed. `ADB`, `VISUALQA_APKS`,
`VISUALQA_LOCK_DIR`, `VISUALQA_OWNER` and `VISUALQA_OUT` stand in for the
options. Reports are text and JSON; the recordings and pictures beside them
are frames of the game and stay out of the repository (`--out` is a private
folder; `visualqa-out/` is ignored).

## What a run on a phone does

1. Takes the phone's lock (`mkdir <lock-dir>/<thor|flip>.lock`, its owner and
   time in `owner.txt`), waiting two minutes at a time while somebody else
   has it, and holds it for the whole run. A suite gives the slot up after
   28 minutes and queues again. A phone kept for somebody else
   (`<thor|flip>.next` naming another owner than `--owner`) is not taken
   even when it is free; the one it names takes it and removes the file.
2. Installs the test app and its game code under the package suffix `.vq`
   (`<game>.vq.apk`, `<game>-game.vq.apk`: the debug build, which reads
   `options.txt`), fresh, beside the owner's app. The executables are copied
   from the owner's installed game, which is only read, and the game is
   played from the disc image in the phone's Download folder. Advanced
   Warfare is not read from the owner's app (its install is eleven
   gigabytes): its two executables and its install disc's two packages are
   pushed once to `/sdcard/Download/visualqa-aw` (from a set-up checkout:
   `aw/tu0/default.xex`, `default_mp.xex` and the two files under
   `aw/install/content/`), and it is played from the Game Disc's image in
   Download like the others. The two packages are moved into the test
   app's `dlc` folder for the run and back when the app is removed: the
   launcher starts a game only when its setup is whole, which for Advanced
   Warfare means the install disc's packages beside the game folder, and
   a copy would be four gigabytes a run. A game whose image or files are
   not on the phone is skipped and said.
3. Writes the scenario's switches (`MW2_CONSOLE`, `MW2_INPUT_SCRIPT`,
   `MW2_RUN_SECONDS`, `MW2_REPORT`, `MW2_REPORT_SECONDS`, `MW2_STUTTERS`,
   `MW2_DUMP_FRAMES`) and starts the title.
4. Records the screen with `adb shell screenrecord` (H.264, 12 Mbit/s) from
   twelve seconds before play to the end of the run, in segments of at most
   170 seconds that overlap by three. The phone's own log (`logcat`) has
   every line of the game's with the phone's time on it; the lines that name
   their second of the run ("in play at 30.339 s", every `STUTTER`) place
   the run's clock, and so each segment, to a tenth of a second.
5. Pulls the recording, `game.log` and the frames the game wrote itself, and
   removes the test app and everything it wrote.
6. Checks them (below) and writes `report.txt`, `report.json` and
   `pictures/` under `<out>/<device>/<scenario>/`.

A first visit is what is tested: the app is fresh, so its caches are the
ones a new player has.

### Building the test apps

From a worktree with both things `android/fetch_deps.sh` fetches (the Turnip
driver asset and `third_party/libadrenotools`):

    cd android
    ./gradlew -PappSuffix=.vq :app:assembleMw3Tu0Debug          # and Mw2Tu0, Cod4Tu0, AwTu0
    ./gradlew -PappSuffix=.vq -Pgame=mw3 -PgameLibraries=<built game libraries> :game:assembleDebug

named `<game>.vq.apk` and `<game>-game.vq.apk` in one folder. A build
without those two falls back to the phone's own Vulkan driver, whose picture
on an Adreno is not the one a release shows; the run then fails its `driver`
check (`--driver system` is for a run that means to test that driver).

## The two sources of frames

| source | what it is | used for |
|---|---|---|
| the screen recording | every picture the display showed, through H.264 | flicker (frame against both neighbours), frozen pictures, blocks, black and white frames, missing geometry |
| the game's own frames (`MW2_DUMP_FRAMES`, one in 240) | exact to the bit and without the encoder, but four seconds apart | blocks and black, white and one-colour frames again, on a picture nothing has touched |
| the log (`MW2_REPORT_SECONDS`, `MW2_STUTTERS`) | each second's frames and longest frame, every stutter and stall, from the game's side | pacing, stalls, pop-in, which seconds were play, which driver |

At 120 Hz `screenrecord` drops frames of its own, so no pacing is read from
the recording: a dropped frame there can hide a flash but cannot make one.
On Windows there is no recording: the game writes one frame in six
(`--dump-every`) and they are checked as a recording a tenth of a second a
frame, so a flash of one frame is seen one time in six, and exactly. The
frames are deleted after the run (a thousand are gigabytes); the pictures
of the faults are kept. A build older than `MW2_REPORT_SECONDS` has no line
a second in its log, and its pacing is then not judged.

## The checks

Each check's numbers are in `tools/visualqa/detectors.py` (`Thresholds`)
with what they were measured against.

| check | fails when | measured |
|---|---|---|
| `flicker` | the whole frame's brightness steps 5 levels or more away from both neighbours for one frame, every region with it (the tenth that stepped least by 0.4 of the middle one's step) | Advanced Warfare's gamma fault: 13.4 levels on each of 33 flashes, the least region 0.56 to 0.79 of the middle; with Brightness four steps down 0.4 levels; the largest clean step 2.4; a weapon's flash, sparks and explosions step 6 to 40 levels with the least region at 0.00 to 0.25 |
| `local-flicker` | where the scenario is calm, a patch at least 20 screen pixels thick and 0.4% of the frame (more in a turn: +12% of the share of the picture that is moving, measured between the two neighbours and against the frame before, whichever is larger) differs from both neighbours for one frame and is in neither of them at another place, and is not an upright band (at most 0.25 of the width, from the top of what is seen down 0.7 of the height or more, with 0.2 of the picture moving: a post passed too close to be in a neighbouring frame); once in a run | the sun shadow's wedge 3 to 29% of the frame; lit patches through a doorway 0.8 to 0.9%; Dome's shadow lost for a frame in a turn 8.0% with 43% moving; Rust with half its draws missing for a frame 7.5%; clean play standing still at most 0.14%; a beam crossing a dark shed in a jump at 120 Hz 2.6% with 20% moving |
| `blocks` | eight or more squares of one size (26 to 38 px a side, as wide as high to within 3 px, the middle one filling 0.86 of the box round it) stand in rows and columns on one lattice, in three frames | the lattices of Call of Duty 4's Bog, Modern Warfare 3's Dome and Modern Warfare 2's S.S.D.D. on the phone's own driver: 9 to 59 blocks 30 to 36 px a side, regularity 0.94 to 1.00, filling 0.87 to 1.00 of their boxes (Dome's 0.86 or more in 436 of 477 frames); clean play at most 5 alike and not in rows; Solar's honeycomb wall and a grille's slots stand in rows and are 46 by 42 and 26 by 18, and the honeycomb from further off and a rack of pipes are a block's size and round, filling 0.81 to 0.85; a perforated screen on Riot, Rust's wire fence and Dome's window grilles are square and in rows, 16 to 22 px |
| `black`, `white`, `uniform` | in play, 97% of the frame is black, or 60% white or nothing in it darker than level 150, or its spread is under 3 levels, for five seconds | a level's own fades are under five seconds (Black Tuesday's: four) |
| `missing` | where the scenario is calm, four or more neighbouring regions (of 16 by 9) go from textured to one flat colour between two frames while no more than a fifth of the rest of the picture changes and no region beside them was that colour without texture the frame before, twice in a run; a single column of them all one colour (to 8 levels) does not count | the wedge does, with a tenth of the rest changing; a walk up to a dark wall goes flat with 0.45 to 0.58 of the rest changing; a bare wall sliding in as an edge slides out in a turn is the colour of the region beside it to 1 to 4 levels; a pillar strafed past close by fills one column of five or six regions with its one colour, 0 to 4 levels apart |
| `frozen` | in play, the picture does not change for two seconds while the log says frames were made | |
| `stall` | the log has a `STALL` (a second or more without a world frame) in play | Call of Duty 4's first visit to The Bog: 9.9 s |
| `unfinished` | the log has no closing report: the game stopped or was killed | |
| `pacing` | after the first 30 seconds of play, more than 5% of the seconds have under half the frames the run is held to or a frame of 250 ms | |
| `pop-in` | after the first 30 seconds of play, the renderer still stops to read new textures in more than half the seconds | a fresh install reads them in about a tenth |
| `driver` | the run was not on the driver asked for (the Turnip the build carries, unless `--driver system`) | |

`first-visit` (slow seconds in the first 30 of play) and `pop-in` below its
limit are said and do not fail: a fresh install compiles and reads what it
has never seen.

What is left out, and why:

- **The HUD and the touch controls** are masked (`frames.py`, `MASKS`): the
  minimap, the score, the weapon and the compass change every frame and say
  nothing of the world. A run's own app has its touch controls and
  statistics line off; `--mask touch` is for a recording that has them.
- **Loading screens and menus.** The log has a line for every second of
  play; the play-only checks (everything but `flicker`) judge those seconds
  alone, a second in from each end, and nothing before the scenario's own
  start of play. A menu may flash by design (Modern Warfare 3's does).
- **Fights.** A weapon's flash, sparks, an explosion and the jolt of a hit
  are single frames that differ from both neighbours, by design, and no
  number tells them from a shadow that flickers. A scenario says from which
  second it has a fight on screen (`combat_from`): before it
  `local-flicker` and `missing` judge, from it they only remark (a `note`
  with the count and the largest patch's picture). So a match is walked
  through empty first and its bots join afterwards, and the player fires in
  the fight alone. `flicker` judges throughout: an effect lights the
  regions near it, the gamma fault every region.
- **Things that move.** A post, a shadow's band or a ribbed wall passing
  close in a fast turn moves further in a frame than it is thick, and where
  it stands in a frame neither neighbour has it; the whole view does the
  same at the frame a turn reverses in and at a hit's jolt. Such a patch is
  looked for in both neighbours up to 72 pixels across and 40 down (of a
  frame 480 wide) from where it is, and is not counted when one of them has
  it: with it there the difference falls to 0.13 to 0.60 of what it was
  (under 0.7 is taken; what the HUD covers is left out of the comparison) and the two pictures agree in shape (a correlation
  of 0.35 or more). The two faults found in runs fell to 0.93 and 0.96:
  what flashed is nowhere in the neighbours.
- **A game at 30 frames a second** is recorded twice a frame; repeated
  frames are dropped before a frame is compared with its neighbours.

## The scenarios

`visualqa.py list` prints them; they are in `tools/visualqa/scenarios.py`.
Each loads a level or a match from the console and walks the player through
it by a pad script (forward, turns, a sidestep, a jump, and in a fight a
burst of fire every twelve seconds).

| scenario | what | single-frame checks judge |
|---|---|---|
| `mw2-campaign` | Modern Warfare 2, S.S.D.D.: the camp in sunlight | the whole run |
| `mw2-bots` | Modern Warfare 2, Rust: empty for 40 s, then six bots | the empty part |
| `mw3-campaign` | Modern Warfare 3, Black Tuesday: the street fight | nothing: a fight throughout |
| `mw3-bots` | Modern Warfare 3, Dome: empty for 40 s, then six bots | the empty part |
| `mw3-survival` | Modern Warfare 3, Survival on Dome, the first 35 s of play: the first wave kills a player who only walks (`god` does nothing in Special Ops), and the results screen that stands after it is not judged | nothing |
| `cod4-campaign` | Call of Duty 4, The Bog (the first visit that stalled) | nothing |
| `cod4-bots` | Call of Duty 4, Crash: empty for 40 s, then six bots | the empty part |
| `aw-campaign` | Advanced Warfare, Induction | nothing |
| `aw-match` | Advanced Warfare, Riot in a private match, empty | the whole run |
| `aw-shadows` | Advanced Warfare, Solar in a private match, empty, with the sun's shadows on (`MW2_SHADOWS=1`; they are off by default where a phone runs above 60 frames a second or has an Adreno 6xx) and the player standing still twelve seconds in every twenty-four | the whole run |

Every other check (`flicker`, `blocks`, black and white frames, `frozen`,
stalls, pacing, the driver) judges every scenario from its start of play to
its end.

## Where the suite stands

On 26 November 2026, on the release's commit `821d28f`, every run on the
bundled Turnip; the figures are the frames a second in play and the longest
frame.

| scenario | AYN Thor (120 Hz) | Retroid Pocket Flip 2 (60 Hz) |
|---|---|---|
| `mw2-campaign` | PASS, 118.7, 59 ms | PASS, 60.1, 67 ms |
| `mw2-bots` | PASS, 119.0, 41 ms | PASS, 60.1, 58 ms |
| `mw3-campaign` | PASS, 117.2, 46 ms | PASS, 59.8, 83 ms |
| `mw3-bots` | PASS, 117.4, 77 ms | FAIL, 59.9, 62 ms: `local-flicker` at 93.5 s, the check's mistake (posts on a container passing in a fast turn) |
| `mw3-survival` | PASS, 117.9, 44 ms | PASS, 60.1, 77 ms |
| `cod4-campaign` | PASS, 118.0, 56 ms | PASS, 59.9, 90 ms |
| `cod4-bots` | PASS, 119.2, 74 ms | PASS, 60.1, 145 ms |
| `aw-campaign` | PASS, 118.3, 50 ms | PASS, 60.4, 47 ms |
| `aw-match` | PASS, 118.4, 73 ms | PASS, 60.4, 92 ms |
| `aw-shadows` | FAIL, 118.6, 74 ms: `blocks` at 185.6 s, the check's mistake (a rack of round pipes on Solar) | PASS, 60.3, 155 ms |

No fault of the game's in the twenty runs. The two single-frame faults
earlier runs found are not in them: Rust's frame with half its draws
missing (the Flip 2 on `82c579e`, one frame in 40 s of calm play; mended
in `7468331`) and the sun's shadow lost for a frame, which on Solar with
the shadows on was 291 frames in 100 s on the Flip 2 before `f2311ad` and
none after it (none in 100 s on the Thor before it either).

The check's own mistakes found on the way are mended and in the labelled
set or the tests: posts, a shadow's band and a ribbed wall passing in fast
turns, a view reversing its turn, a beam crossing a dark shed in a jump and
a pillar passing behind the minimap (`local-flicker`); Survival's results
screen (`frozen`); Solar's honeycomb wall, its grille and its round pipes
(`blocks`) and its bare walls sliding past (`missing`). A run that fails is
looked at in its pictures before it is called a fault.

Solar's `blocks` on the Thor was the last of them (27 November 2026). Four
runs of `aw-shadows` there, two on `fff0519` and two on the release's
`8b7d1a6`, counted the honeycomb wall or the pipes as a lattice in 0 and 5
frames and in 1 and 1 (three frames fail a run), and a run before them in
23: the same wall on both builds, so the check's mistake and nothing the
merge of Black Ops III changed. None of the four had a single-frame patch
over 0.45 of what `local-flicker` asks; the patches at the picture's edge
that three earlier runs were failed for (a post or a wall's edge in a fast
turn) did not come again, and no recording of them was kept to measure.

On 27 November 2026, with the apps built at `fff0519` and the checks as
they are here (`94045b8`, merged since, is not in these runs):

| scenario | AYN Thor (120 Hz) | Retroid Pocket Flip 2 (60 Hz) |
|---|---|---|
| `mw2-campaign` | PASS, 118.5, 44 ms (run again: the first run's recording was not pulled, its folder gone from the PC) | FAIL, 60.1, 58 ms: `local-flicker` at 103.8 s, a rifle's flash at the firing range behind the squad, which the scenario calls calm |
| `mw2-bots` | FAIL, 118.6, 52 ms: `local-flicker` at 89.5 s, the check's mistake (the ribbed wall of a dark shed in a turn) | PASS, 60.2, 58 ms |
| `mw3-campaign` | PASS, 117.0, 97 ms | PASS, 59.9, 85 ms |
| `mw3-bots` | PASS, 117.2, 77 ms | PASS, 59.7, 73 ms |
| `mw3-survival` | PASS, 118.2, 41 ms | PASS, 60.0, 77 ms |
| `cod4-campaign` | PASS, 117.5, 58 ms | PASS, 59.9, 75 ms |
| `cod4-bots` | PASS, 118.6, 62 ms | PASS, 60.1, 133 ms |
| `aw-campaign` | PASS, 118.3, 52 ms | PASS, 60.4, 56 ms |
| `aw-match` | PASS, 118.4, 61 ms | FAIL, 60.1, 274 ms: `local-flicker` at 183.4 s, the check's mistake (pillars at the picture's right edge in a fast turn) |
| `aw-shadows` | FAIL, 118.3, 102 ms: `local-flicker` at 131.9 s, the glow under a strip light brighter for single frames six times in a third of a second, the view standing; it looks like the lamp's own flicker and is not proven to be | PASS, 60.2, 168 ms |

Sixteen of the twenty pass. Three of the four that fail are looked at in
their pictures and are not the game's; Solar's strip light is open. The pillars at the picture's edge are the
mistake three earlier runs of `aw-shadows` were failed for, and this time
its recording is kept (`private-captures\releaseready`), for the check to
be mended against.

On 27 November 2026, with the apps built at `1d97933` (every texture the
title rewrites read where a frame first binds it) and the checks as they
are here:

| scenario | AYN Thor (120 Hz) | Retroid Pocket Flip 2 (60 Hz) |
|---|---|---|
| `mw2-campaign` | PASS, 115.3, 47 ms | PASS, 60.0, 68 ms |
| `mw2-bots` | PASS, 118.8, 43 ms | PASS, 60.1, 64 ms |
| `mw3-campaign` | PASS, 115.0, 71 ms | PASS, 59.8, 83 ms |
| `mw3-bots` | PASS, 116.9, 77 ms | PASS, 59.8, 77 ms |
| `mw3-survival` | PASS, 117.7, 73 ms | PASS, 59.9, 70 ms |
| `cod4-campaign` | PASS, 118.0, 70 ms | PASS, 59.6, 84 ms |
| `cod4-bots` | PASS, 118.8, 54 ms | PASS, 60.0, 83 ms |
| `aw-campaign` | PASS, 118.4, 61 ms | PASS, 60.4, 50 ms |
| `aw-match` | PASS, 117.8, 84 ms (run again: the first run, 117.6, failed `blocks` at 165.5 s on a perforated screen, the check's mistake, mended and labelled) | PASS, 60.2, 92 ms |
| `aw-shadows` | PASS, 118.2, 96 ms | PASS, 60.2, 149 ms |

All twenty pass. The single-frame patches the fights of Modern Warfare 2,
Modern Warfare 3 and Call of Duty 4 have (not judged there) were looked at:
muzzle flashes, sparks and explosions, no lighting or texture of one frame.
Induction's drop pod, counted as frames 40 levels or more brighter than
both neighbours in the whole recording: none in three runs on the Thor (117
frames a second recorded) and none in one on the Flip 2, where a run before
`7ef5f79` had three, one of them the pod's 202 levels against 23.

On 27 November 2026, with the apps built at `c41a440` (graphics presets,
performance reports, looking by touch) and the checks as they are here:

| scenario | AYN Thor (120 Hz) | Retroid Pocket Flip 2 (60 Hz) |
|---|---|---|
| `mw2-campaign` | PASS, 117.1, 43 ms | PASS, 60.0, 69 ms |
| `mw2-bots` | PASS, 118.9, 45 ms | PASS, 60.2, 57 ms |
| `mw3-campaign` | PASS, 116.2, 97 ms | PASS, 59.8, 88 ms |
| `mw3-bots` | PASS, 117.4, 70 ms | PASS, 59.8, 67 ms |
| `mw3-survival` | PASS, 117.9, 42 ms | PASS, 59.9, 71 ms |
| `cod4-campaign` | PASS, 118.0, 57 ms | PASS, 59.6, 76 ms |
| `cod4-bots` | PASS, 119.0, 49 ms | PASS, 60.0, 76 ms |
| `aw-campaign` | PASS, 118.3, 52 ms | PASS, 60.4, 50 ms |
| `aw-match` | PASS, 117.7, 76 ms | PASS, 60.2, 301 ms (a first visit's hitch at 198.3 s: 71 shaders and 160 textures new) |
| `aw-shadows` | PASS, 117.9, 98 ms (third run; the first, 118.2, failed `local-flicker` at 134.7 s, a real fault, and the second, 118.2, failed `missing` at 152.4 and 153.8 s, the check's mistake, mended) | PASS, 60.2, 168 ms |

No frame rate fell from the `7c8a5f1` runs by more than 0.3 a second;
the Thor's S.S.D.D. and Black Tuesday, 115.3 and 115.0 then, are 117.1 and
116.2. The fights' single-frame patches are muzzle flashes, explosions and,
on Crash, chips of a wall shot at close range.

The fault: in the first Thor run of `aw-shadows` the held weapon and arms
are gone for one frame but for a sight and a magazine. Seven pipelines for
them were new at once on the fresh install's first visit, more than the six
workers, so their draws were skipped. That rule and its skip counts are
the same at `7c8a5f1`. It is a known fault of a first visit, written up in
[rendering.md](rendering.md#pipelines), and its recording is in the
labelled set. The second run's `missing` was a dark pillar, strafed past
close by, filling one column of regions with its own colour. The check
now lets a single column of one colour pass.

On 27 November 2026, with the apps built at `f39fd7b` (translations,
invert switches, driver fixes, aim when firing, floating stick, the pad's
slot, CPU clusters by capacity) and the checks as they are here:

| scenario | AYN Thor (120 Hz) | Retroid Pocket Flip 2 (60 Hz) |
|---|---|---|
| `mw2-campaign` | PASS, 114.8, 48 ms | PASS, 60.0, 64 ms |
| `mw2-bots` | PASS, 118.1, 48 ms | PASS, 60.1, 58 ms |
| `mw3-campaign` | PASS, 114.9, 53 ms (failed `black` at 150 s, the check's mistake, mended) | PASS, 59.9, 84 ms (the same at 151 s) |
| `mw3-bots` | PASS, 116.1, 53 ms | PASS, 59.9, 62 ms |
| `mw3-survival` | PASS, 117.3, 52 ms | PASS, 60.0, 68 ms |
| `cod4-campaign` | PASS, 116.4, 69 ms | PASS, 59.7, 77 ms |
| `cod4-bots` | PASS, 118.5, 56 ms | PASS, 60.0, 77 ms |
| `aw-campaign` | PASS, 117.3, 227 ms | PASS, 60.3, 55 ms |
| `aw-match` | PASS, 117.7, 70 ms | PASS, 60.3, 95 ms |
| `aw-shadows` | PASS, 118.2, 103 ms (second run; the first ended at 57 s, the debug build's `STALL` stack dump faulting in the unwinder, as in BO3-BRINGUP.md) | PASS, 60.2, 163 ms (second run; the first failed `local-flicker` at 209 s, a fault) |

Black Tuesday's walk on this build ends, on both phones alike, in an unlit
doorway at 149 to 161 s: walls at levels 3 to 8, textured (they vary by 1.9
to 3.0 levels among the pixels under 16). `black` now wants the black to be
flat as well (`black_flat`); a frame drawn black varies by none. The Flip's
first `aw-shadows` run went into a dark room on Solar and a flat pale blue
band swept the view near 206 s and from 208.5 to 209.1 s while the room
stood still: taken for the known sun-shadow flicker (AW-BRINGUP.md, the
cause open, with the sun's shadows on), not compared at that spot on
`c41a440`'s apps, whose rerun did not walk there; its recording is in the
labelled set as a fault. The Thor ran 0.5 to 2.3 frames a second below the
`c41a440` tables in every scenario, with the same CPU clusters (`cpu:
cores 80 the fastest, 78 the title's, 07 the background's` on the Thor,
`80, 70, 0F` on the Flip 2, by capacity: as by clock before) and the same
graphics chip and renderer times a frame. Run back to back on the Thor
that evening, S.S.D.D. made 114.8 with `c41a440`'s apps and 114.4 with
these: the phone's state that day, not the code.

The rest of that release's gate, the same evening, on the same apps
(`81cf718` changes no app code):

- **The update** (Thor, as a player has it, docs/android-releases.md):
  the v0.1.12 (`27754dc`) release-signed `.ru` app built Modern Warfare 3's
  game code on the phone in 262 s; the v0.1.13 (`f39fd7b`) app went over it
  by `pm install -r` from `/data/local/tmp`. No rebuild, the build report and
  `saves/` byte for byte the same, the profile read back, both libraries
  "built on this device ... which this app takes", a campaign game started.
- **Controller save** (Thor, MW3, the built-in pad): slot 0 (player 1), the
  save written, and after a restart Resume Game, Yes loaded
  `sp_ny_manhattan`. The Flip 2 had shown it in CoD4.
- **Aim when firing** (`tools/aim_fire.py`): Thor in MW3, LT 6 to 24 ms
  after the finger and RT 199 to 200 ms after LT, the sights up in the
  picture; Flip 2 in MW2, RT 200 to 205 ms after LT. A tap still shoots, Aim
  held makes Fire a plain RT, and with the setting off Fire is RT alone, on
  both. No sight timing was read: MW3 writes no `look: yaw` lines (it turns
  by the stick) and MW2's opening player holds no weapon yet (zoom stays 1).
- **Floating stick and auto-sprint** (`tools/move_stick.py`): Thor in MW3,
  Flip 2 in MW2: the stick under the finger to the pixel, sprint 299 to
  300 ms after the edge and let go on the way back, nothing with both off.
- **Invert horizontal and vertical** (Thor, MW3, `tools/invert_look.py`):
  the turns the app hands the game (`look: turn`) were +26.04 right and
  +12.98 down with neither, -26.04 and +12.98 with horizontal, +26.04 and
  -12.98 with vertical. The script itself reports `WRONG` (0.00) because it
  reads the view's angles from `look: yaw` lines, which only titles whose
  touch look goes into the view directly write; MW3's campaign turns by the
  stick (`android/input.cpp`, `Turn`).
- **Another language**: the app set to German by its own language
  (`cmd locale set-app-locales`, the system left at en-US): the start hint
  and the in-game menu in German.
- **The blue band** on Solar: two more Flip 2 runs of `aw-shadows` passed
  with no `local-flicker`; at 206 to 209 s one was elsewhere on the map, the
  other in a dark interior with no band. Not seen again, so not compared
  with `c41a440`.

On 28 November 2026, the release's suite on `c677978` (MW3/MW2 v0.1.14,
CoD4 v0.1.9, AW v0.1.7) passed every row but the Thor's `mw2-bots`, which
failed `local-flicker` twice in the empty part, and neither is the
release's doing:

- **89.7 s**, the ribbed wall of the dark shed in a turn: the check's
  mistake the `fff0519` run was failed for at 89.5 s (the table above).
- **85.4 s**, a lit patch on a container's wall for one frame (and a second
  one 50 ms later, under the check's size). The sun's shadows came on by the
  adaptive rule at 84.4 s (`sm_enable 1`), and 14 pipelines were found
  missing at once in that second, more than the six workers: the known case
  of [rendering.md](rendering.md#pipelines) where no wait is begun and a
  colour draw goes for a frame. Every Thor run of this scenario has that
  burst (10 to 24 misses) when the shadows come on; whether a frame shows it
  depends on what is in view.

Modern Warfare 2's code is the same on `1f5326a`, whose runs passed: its
game library is byte for byte the same in both apps, `runtime/mods.cpp` is
built only where `MW2_GSC` is (Modern Warfare 3 and Advanced Warfare's
multiplayer), and the two apps' libraries differ only in the build's paths
and the addresses they move. Six runs on the Thor, interleaved, `1f5326a`
against `0c28bfb` (= `c677978` + a CI fix): all six pass, the shadows
coming on at 84.4 s in four of them (the other two at 108 and 110 s). Known
and not blocking; the reports and the failing run's recording are in
`private-captures\release0114\mw2bots_ab`.

On 28 November 2026, Black Ops III's work toward 120 on the Thor
(`bo3-thor120`, BO3-BRINGUP.md) against android before it merged: apps
built from the branch (`.bm`) and from `1f5326a` (`.bmh`), the same game
libraries in both, each scenario run with both apps back to back, the order
alternating. Of its shared runtime changes, the poll's yield, the spinning
yield's sleep and Advanced Warfare's core layout are Black Ops III's alone
(`T_POLL_YIELD`, `T_YIELD_SLEEP_US`, `T_FRAME_PROCESSORS`); the rest reach
every title: a wait keeps the objects it names on the stack, the scene is
drawn smaller only while the chip is at work 85% of the window or more and
steps back up against the frames' own period, the performance line gives
the 1% low, `KeSetAffinityThread` is logged, and the main thread is given
the title's cores again at the 1200th frame (no title but Black Ops III
moves its main thread, by those log lines, so for the four games the mask
is the one it already has).

| scenario | Thor, branch | Thor, android | Flip 2, branch | Flip 2, android |
|---|---|---|---|---|
| `mw2-campaign` | PASS, 115.7, 44 ms | PASS, 114.4, 45 ms | PASS, 60.0, 69 ms | PASS, 60.0, 67 ms |
| `mw2-bots` | PASS, 118.5, 46 ms | PASS, 118.7, 53 ms | PASS, 60.2, 58 ms | PASS, 60.1, 64 ms |
| `mw3-campaign` | PASS, 115.1, 49 ms | PASS, 115.4, 47 ms | PASS, 59.7, 84 ms | PASS, 59.8, 105 ms |
| `mw3-bots` | PASS, 116.8, 81 ms | PASS, 116.7, 60 ms | PASS, 59.9, 83 ms | PASS, 59.7, 69 ms |
| `mw3-survival` | PASS, 118.2, 52 ms | PASS, 117.9, 42 ms | PASS, 60.1, 76 ms | PASS, 59.9, 76 ms |
| `cod4-campaign` | PASS, 117.0, 71 ms | PASS, 117.6, 78 ms | PASS, 59.5, 199 ms; again 59.3, 205 ms | PASS, 59.4, 106 ms; again 59.2, 740 ms |
| `cod4-bots` | PASS, 118.6, 61 ms | PASS, 118.6, 81 ms | PASS, 60.1, 132 ms | FAIL, 60.1, 71 ms: `flicker` at 159.3 s (a muzzle flash on a wall at arm's length) and `local-flicker` at 85.0 s (the held rifle drawn unlit for one frame) |
| `aw-campaign` | PASS, 118.0, 50 ms | PASS, 118.1, 51 ms | PASS, 60.4, 61 ms | PASS, 60.3, 64 ms |
| `aw-match` | PASS, 118.2, 74 ms | PASS, 118.0, 74 ms | PASS, 60.1, 285 ms; again 60.3, 88 ms | PASS, 60.3, 92 ms; again 60.2, 85 ms |
| `aw-shadows` | PASS, 118.3, 110 ms | PASS, 118.3, 102 ms | PASS, 60.0, 158 ms | PASS, 60.2, 152 ms |

No frame rate differs by more than 1.3 a second, and that one (Thor
S.S.D.D.) is the branch's ahead. The Flip 2's long frames are first visits: The Bog's is in its first second of
play on both apps (the title late, 199 to 740 ms), and Riot's 285 ms was 66
shaders and 156 textures new at once, not there on the run after it. The
unlit rifle on Crash is android's app, the branch's run of it clean.

## Calibration

`tools/visualqa/calibration.json` lists the labelled recordings: which
checks must fail each, or that it must pass. The recordings are private
captures; `visualqa.py calibrate --root <folder>` runs the checks over them
and prints the precision and recall. Change a number in `Thresholds` only
with that run beside it, and add a recording of every new fault a player
finds.

On 27 November 2026, with the numbers in `Thresholds` and `frames.py`:

| labelled | recordings | result |
|---|---|---|
| whole-frame flicker (Advanced Warfare's gamma fault: the owner's recording, three reproductions in the lobby, one in a match) | 5 | 5 caught |
| local flicker (a tester's three recordings of the sun shadow on Solar and through a doorway) | 3 | 3 caught |
| single frames of a run that were wrong, in an empty match (Rust with half its draws missing, Dome's sun shadow lost, Solar's held weapon gone while its pipelines compiled) | 3 | 3 caught |
| block lattices on the phone's own driver (two recordings, two single frames) | 4 | 4 caught |
| clean menus and first screens of the four titles, before and after the gamma fix where it changed nothing | 11 | 11 passed |
| a clean match with the player turning fast | 1 | passed |
| fresh runs on the bundled Turnip (S.S.D.D. twice, Rust with bots, Black Tuesday, Survival, The Bog) | 6 | 6 passed |
| empty matches whose posts, ribbed walls and jumps the check once took for flashes (Rust at 120 Hz, Dome at 60 and 120 Hz) | 3 | 3 passed |
| Solar with the sun's shadows on, whose honeycomb wall, grille, pipes and bare walls two checks once took for blocks and missing ground | 2 | 2 passed |
| Riot on the Flip 2, where a pillar passing behind the minimap in a turn was taken for a flash | 1 | passed |
| a post or a pillar passed too close to be in either neighbouring frame (Riot strafed past on the Flip 2 twice, Solar on the Thor) | 3 | 3 passed |
| Riot on the Thor, whose perforated screen of square holes 16 to 18 px a side the block check took for a lattice | 1 | passed |
| Solar on the Thor, where a pillar strafed past close by filled a column of regions with its colour and the missing check took it for ground gone | 1 | passed |

16 of 16 faulty recordings caught (the Flip 2's blue band on Solar, `f39fd7b`, added) and 29 of 29 clean ones passed: recall
1.00, precision 1.00, by recording. The set is small, and nothing in it is
a black world, a washed frame, a frozen picture or missing ground in a
recording of a real run: those checks are tested on drawn pictures alone
(below) until a recording of each is added. The measurements of a
recording are kept beside its report (`measurements_*.npz`, and under
`calibrate/` for the labelled set): delete them after changing
`frames.py`, which measures.

## Tests

`python tests/visualqa_test.py` draws its own pictures (a flash, a wedge, a
lattice, a black screen, a fast turn) and its own log lines, and checks
that each check fails what it should and nothing else.
