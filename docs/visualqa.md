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
   28 minutes and queues again.
2. Installs the test app and its game code under the package suffix `.vq`
   (`<game>.vq.apk`, `<game>-game.vq.apk`: the debug build, which reads
   `options.txt`), fresh, beside the owner's app. The executables are copied
   from the owner's installed game, which is only read, and the game is
   played from the disc image in the phone's Download folder. Advanced
   Warfare's installed files are copied whole.
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
| `local-flicker` | where the scenario is calm, a patch at least 20 screen pixels thick and 0.4% of the frame (more in a fast turn: +6% of the share of the picture that is moving) differs from both neighbours for one frame, twice in a run | the sun shadow's wedge 3 to 29% of the frame; lit patches through a doorway 0.8 to 0.9%; clean play standing still at most 0.14%, in the fastest turn 3.7% with 83% moving |
| `blocks` | eight or more rectangles of one size (16 to 80 px a side) stand in rows and columns on one lattice, in three frames | the lattices of Call of Duty 4's Bog, Modern Warfare 3's Dome and Modern Warfare 2's S.S.D.D. on the phone's own driver: 9 to 59 blocks, regularity 0.94 to 1.00; clean play at most 5 alike and not in rows |
| `black`, `white`, `uniform` | in play, 97% of the frame is black, or 60% white or nothing in it darker than level 150, or its spread is under 3 levels, for five seconds | a level's own fades are under five seconds (Black Tuesday's: four) |
| `missing` | where the scenario is calm, three or more neighbouring regions (of 16 by 9) go from textured to one flat colour between two frames while no more than a fifth of the rest of the picture changes, twice in a run | the wedge does, with a tenth of the rest changing; a walk up to a dark wall goes flat with 0.45 to 0.58 of the rest changing |
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
  regions near it, the gamma fault every region. A frame that is the
  picture before it moved (a hit's jolt) is moved back before it is
  compared.
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
| `mw3-survival` | Modern Warfare 3, Survival on Dome | nothing |
| `cod4-campaign` | Call of Duty 4, The Bog (the first visit that stalled) | nothing |
| `cod4-bots` | Call of Duty 4, Crash: empty for 40 s, then six bots | the empty part |
| `aw-campaign` | Advanced Warfare, Induction (the phones that have its files) | nothing |
| `aw-match` | Advanced Warfare, Riot in a private match, empty | the whole run |

Every other check (`flicker`, `blocks`, black and white frames, `frozen`,
stalls, pacing, the driver) judges every scenario from its start of play to
its end.

## Calibration

`tools/visualqa/calibration.json` lists the labelled recordings: which
checks must fail each, or that it must pass. The recordings are private
captures; `visualqa.py calibrate --root <folder>` runs the checks over them
and prints the precision and recall. Change a number in `Thresholds` only
with that run beside it, and add a recording of every new fault a player
finds.

On 26 November 2026, with the numbers in `Thresholds`:

| labelled | recordings | result |
|---|---|---|
| whole-frame flicker (Advanced Warfare's gamma fault: the owner's recording, three reproductions in the lobby, one in a match) | 5 | 5 caught |
| local flicker (a tester's three recordings of the sun shadow on Solar and through a doorway) | 3 | 3 caught |
| block lattices on the phone's own driver (two recordings, two single frames) | 4 | 4 caught |
| clean menus and first screens of the four titles, before and after the gamma fix where it changed nothing | 11 | 11 passed |
| a clean match with the player turning fast | 1 | passed |
| fresh runs on the bundled Turnip (S.S.D.D. twice, Rust with bots, Black Tuesday, Survival, The Bog) | 6 | 6 passed |

12 of 12 faulty recordings caught and 18 of 18 clean ones passed: recall
1.00, precision 1.00, by recording. The set is small, and nothing in it is
a black world, a washed frame, a frozen picture or missing ground in a
recording of a real run: those checks are tested on drawn pictures alone
(below) until a recording of each is added.

## Tests

`python tests/visualqa_test.py` draws its own pictures (a flash, a wedge, a
lattice, a black screen, a fast turn) and its own log lines, and checks
that each check fails what it should and nothing else.
