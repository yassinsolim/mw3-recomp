# Changelog

All notable changes to the Modern Warfare 2 Android native recompilation project will be documented in this file.

## [Unreleased]

### Fixed
- **Call of Duty 4 no longer stops for good the first time a level is played
  on an AYN Thor** (The Bog, 90 to 110 seconds in, a still picture until the
  game was closed; the second visit played). The sun's occlusion count was
  waited for inside Qualcomm's driver, which leaves a query or two of a first
  visit without a count for ever; the count is now read without waiting once
  its frame has finished, and a query the driver never answers counts
  nothing (`runtime/gpu/vulkan/renderer_frame.cpp`, all games share it;
  [docs/rendering.md](docs/rendering.md#occlusion-queries)). Diagnostics
  builds on Android now write every thread's stack at a `STALL`, which is
  how it was found.

### Added
- **`tools/release/release.py`: a release is made by one script, behind
  gates.** It filters the branch into the four public mirrors (refusing a
  game file, a blob over 20 MB, a path of a game that is not released, or a
  push that is not a fast-forward), pushes the tags, and checks each
  drafted APK itself: the workflows ended well, no recompiled code and no
  game file, the release key, the Turnip package and libadrenotools inside
  the APK, the game code the published app took still taken (or the notes
  say it is built again), the build tools published first, and notes of
  plain text under 1,450 characters. `publish` wants the report of the
  device gates as well and afterwards fetches every download a player's
  app would ([docs/android-releases.md](docs/android-releases.md#the-automated-procedure)).
- **`tools/visualqa`: every test run is recorded and every frame of it
  checked.** `visualqa.py run <scenario> --device thor` starts a title in a
  level or a match with bots, records the screen (`adb shell screenrecord`)
  beside the run's own frame log and the frames the game writes itself, and
  prints PASS or FAIL with a picture of each fault: whole-frame flicker,
  patches lit or lost for single frames (the sun shadow's wedge), a lattice
  of blocks left out of the picture, black, white-washed and one-colour
  frames in play, ground that goes missing between two frames, a frozen
  picture, stalls, slow seconds and pop-in that lasts. Its numbers were set
  on the recordings of the faults players and testers found, and
  `visualqa.py calibrate` measures them again. `--suite` runs a campaign
  level and a match against bots of each title on both phones and is run
  before a release; a run that was not on the bundled Turnip driver fails
  ([docs/visualqa.md](docs/visualqa.md)).
- **Advanced Warfare: the app says which Compatibility Packs are missing.**
  The updated game needs all six, whichever map packs you bought, and with
  one missing it only showed its own notice that required content had been
  added to your downloads, with nothing to do but return to the main menu:
  a tester with one of the six got no further. The app now looks for them
  itself (each package's `dlccompat<n>.id` marker): **Add DLC** names the
  ones still to add and where they are on the console's disk, its entry on
  the start screen reads **ADD DLC · 2 MISSING**, the log has a `map
  packs:` line, and until all six are there **Multiplayer** stays the
  disc's version instead of stopping at the notice.
- **`MW2_TRACE_AIM=1`** (diagnostics builds, Advanced Warfare's disc
  multiplayer): the held weapon's sprint and aim times from its asset, every
  frame of an aim down the sight and how long it took. It answers a
  tester's question: the weapon that stays put after a sprint and is then
  at the sight within a few frames is the title's own timing (the aim runs
  under the sprint's end at the weapon's "from a sprint" rate; the ASM1:
  200 ms of sprint-out, 300 ms of aim, both begun together), the same on
  Windows ([docs/AW-BRINGUP.md](docs/AW-BRINGUP.md#the-aim-after-a-sprint-26-november-2026)).
- **Advanced Warfare: your map packs and Exo Zombies with the phone alone.**
  **Add DLC → Choose a folder**, pick your console's
  `Content\0000000000000000\41560914` folder (the packs in `00000002` and
  the title update in `000B0000`, which they need), and that is all: the
  app copies the packages and then builds the game code of the updated
  multiplayer itself, in the background with the build's notification, from
  your disc and your title update. No PC and no terminal. On a Retroid
  Pocket Flip 2 the copy of 3.5 GB took under a minute and the build
  5 minutes 58 seconds; the campaign and the disc's multiplayer can be
  played meanwhile, and afterwards Multiplayer is the updated game (1.20)
  and Exo Zombies is in the launcher
  ([docs/dlc.md](docs/dlc.md#advanced-warfare-map-packs-and-exo-zombies),
  [docs/ondevice-build.md](docs/ondevice-build.md#the-multiplayer-a-title-update-makes)).
  `setup -Dlc` on a PC still works and is no longer needed. Not played in
  this run: a match on a pack's map from the library built on the phone.

### Documentation
- **Every mode of every game, checked again on Windows, the AYN Thor and
  the Retroid Pocket Flip 2** ([docs/ANDROID-PROGRESS.md](docs/ANDROID-PROGRESS.md#every-mode-of-every-game-25-and-26-november-2026)).
  Split screen plays on the Flip 2 in all four games and in both Special
  Ops (with a simulated second controller: none was at hand); Modern
  Warfare 2's and 3's Find Match and Modern Warfare 2's private match by
  invitation bring the two devices into one match; Advanced Warfare's Exo
  Survival starts from the campaign's menu in the app. Three things that
  looked broken are the games' own: Modern Warfare 2's Find Match timed
  out only behind a host that took a minute to load on an overloaded PC;
  Call of Duty 4's console `map` needs Start pressed on the title screen
  first ([docs/gameplay.md](docs/gameplay.md#loading-a-level-from-the-command-line));
  and the first run's Brightness screen takes A once the slider was moved.

### Changed
- **An app is no longer built without its graphics driver by accident.**
  The apps bundle the Turnip driver and load it through libadrenotools;
  neither is in the repository (`android/fetch_deps.sh` fetches both). A
  tree without them built an app all the same, which ran on the phone's
  own driver and said so only in its log. On the AYN Thor that is a picture
  in 32-pixel blocks, missing ground and 50 to 80 frames a second where
  Turnip gives 119: test apps built that way were taken for a regression of
  the renderer on 26 November 2026, and none was there (Modern Warfare 3's
  multiplayer with six bots on current code and Turnip: clean, 118.8 frames
  a second). The Gradle build of an app now stops when either piece is
  missing and names it (`bundledDriverCheck`), a tree configured before
  libadrenotools was fetched is configured again, and CMake refuses one
  without it. `-PwithoutBundledDriver=true` builds an app for the phone's
  own driver on purpose; `setup.cmd` passes it, and says so, when the
  driver cannot be downloaded. A release never lacked the driver: the
  workflows fetch it.
- **Modern Warfare 2 holds 120 better: no sun shadows above 60 frames a
  second**, as the other three games already had it (**Settings → Graphics
  → Sun shadows → On** keeps them; at 60 and under nothing changes). On
  the AYN Thor its heaviest views, Team Player's, ran at 80 to 90 frames a
  second with them and run at 102 to 112 without; S.S.D.D. spent 39 seconds
  of 160 under 115 and spends none; the Gulag 63 and 17
  ([docs/android.md](docs/android.md#adreno-7xx-snapdragon-8-gen-2-class)).
- **An update of the app no longer asks you to build the game code again
  unless the code really changed, and then does it for you.** Nothing has
  to be built to get this: the game code you have is taken as it is, in all
  four games.
  - The app checked the game code against the tools it was made with (the
    recompiler, its patch, the configuration), so a change of those stopped
    every game, Modern Warfare 2 and Call of Duty 4 included in November
    although their code came out the same. It now checks the code itself,
    the SHA-256 of what the recompiler wrote (`generated-code`): an update
    whose code is the same costs nothing, and other code is still refused,
    by the runtime, before any of it runs
    ([docs/game-library.md](docs/game-library.md#what-they-agree-on)).
  - When an update does expect other code, an app that builds it on the
    device starts by itself the moment the update is installed, in the
    background with a notification (**Updating the game code**), from the
    disc image chosen before, and compiles only the files that came out
    different. On the AYN Thor with Modern Warfare 2: 22 seconds for a
    change of one function (10 of 111 files), with the app never opened,
    against 3 minutes 14 seconds for both titles from nothing
    ([docs/ondevice-build.md](docs/ondevice-build.md#after-an-update-of-the-app)).
    It runs on the charger or above a battery charge you choose (**Settings
    → Update the game code by itself**, 30% unless changed; it can be turned
    off), and memory and heat hold it back as before.
  - The build fetches and installs the toolchain package itself (the
    separate button is gone). Android asks once whether the app may install
    it; later toolchains replace it with no question.
  - `setup\setup.cmd -GameOnly` (`setup/setup.sh --game-only`) after a
    `git pull` compiles only the files the recompiler wrote differently,
    and installs nothing when the phone has that code already.
- **Advanced Warfare plays from its two disc images where they are.** It
  copied both discs into the app, 12 GB on the device's own storage; now
  **Play from the disc image** is the default, as in the other games: the
  game disc is read through the image, and the install disc's two content
  packages are read in place inside the second image (`MW2_INSTALL_DISC_IMAGE`,
  `ReadVolume::OpenPackage` from an offset;
  [docs/runtime.md](docs/runtime.md)). The app keeps 60 MB of its own.
  Copying is still there, the choice is kept (it used to flip to Copy by
  itself), and **Settings → Change where the game is read from** switches an
  installed game over and offers to remove the copies (11.99 GB on the
  Retroid Pocket Flip 2). Measured there with both images on its SD card
  against the copies on its own storage: Seoul 14.0, 13.7 and 13.9 s from
  the images and 13.9 and 13.7 s from the copies (from `devmap seoul` to
  the first frames of the level), Retreat 7.7, 7.8 and 7.2 s against 7.8,
  7.5 and 7.1 s (from Start Game to the team menu over the map)
  ([docs/AW-BRINGUP.md](docs/AW-BRINGUP.md#played-from-the-two-images-25-november-2026)).
- **Setup installs only on a phone it was told.** A run that asks nothing
  (`-Yes`, `--yes`, or no terminal) installs only on the phone `-Device`
  (`--device`) names, and on none when that phone is not connected: it
  used to take the only phone connected, and another in place of a named
  one that was missing. `-NoInstall` (`--no-install`) builds and touches no
  phone at all. At the keyboard nothing changes: you are asked.
- **The game code has to be built again, once, for all four games.** After
  this update the game does not start with the game code you have: the app
  says that it was built for another version of the app and offers **Build
  on this device** in the same message (a few minutes; your game files and
  saves are kept, and no disc image is asked for). The app fetches new
  build tools first (`toolchain-v2`, 43 MB, over the old ones). With a PC
  instead: `git pull`, then `setup\setup.cmd -GameOnly` (`-Game mw2`,
  `-Game cod4`, `-Game aw`; on Linux `bash setup/setup.sh --game-only`),
  which builds the recompiler again and recompiles by itself. Why: the
  recompiler kept every register of half of Advanced Warfare's and Modern
  Warfare 3's functions in memory for one bit of the processor's state
  that one routine writes
  ([docs/building.md](docs/building.md#local-changes-to-xenonrecomp)).
  With that bit left where it is, Advanced Warfare's Fusion on the AYN Thor
  goes from 115.6 frames a second to 118.8, from 82.9% of frames on time to
  93.1% and from 356 stutters in seven minutes to 95; Modern Warfare 3's
  Black Tuesday there from 88 stutters to 31 and a match with six bots from
  69 to 30; nothing is slower on the Retroid Pocket Flip 2
  ([docs/rendering.md](docs/rendering.md#more-than-60-frames-a-second)).
  Modern Warfare 2's and Call of Duty 4's code comes out the same, but the
  recompiler is part of what the app checks the game code against, so
  theirs is rebuilt too
  ([docs/android-releases.md](docs/android-releases.md#when-the-game-code-has-to-be-rebuilt)).
- **A game code the app refuses leads to the build**: the message's button
  opens **Build the game code** where the app can build it, and the text
  names the device first and the PC second.
- The app's **Open the guide** opens the README's quick start (the link
  named a section no README has), and setup's last line for Advanced
  Warfare names its README.

### Added
- **Advanced Warfare's map packs with one command on Linux:
  `setup.sh --dlc <folder>`.** A tester spent an evening on Linux Mint
  trying to get his own map packs going: `setup.sh` had no `--dlc` (only
  Windows' `setup.cmd -Dlc`), and the updated multiplayer he then built by
  hand stated another build than the app asks for, so the app would not
  load it. `bash setup/setup.sh --game aw --iso <game disc> --install-iso
  <install disc> --dlc <your 41560914 folder>` now does what Windows does:
  it looks at the folder first (no title update in it stops in seconds,
  saying what to copy), makes the update's executable from your disc's and
  your update's patch, recompiles it, builds it into the game code beside
  the other two, checks that it fits the published app and copies your
  packages to the phone. With no phone on USB (`--no-install`, or none
  connected) the last lines say what to carry over by hand. Advanced
  Warfare's README has a section for the three ways, the app's **Add DLC**
  first ([README-AW.md](README-AW.md#map-packs-and-exo-zombies),
  [docs/dlc.md](docs/dlc.md#advanced-warfare-map-packs-and-exo-zombies)).
- **What holds the AYN Thor under 120, scene by scene**, in all four games
  ([docs/android.md](docs/android.md#adreno-7xx-snapdragon-8-gen-2-class)):
  seventeen scenes measured, each thread and the graphics chip beside the
  frame rate. Everything runs at 115 to 119 but Modern Warfare 2's Team
  Player and the Gulag's heaviest seconds, held by the thread that reads
  the game's draw commands and behind it by the graphics chip. Newer
  graphics drivers (Mesa 26.3.1 and Mesa's main branch, built for this),
  the driver's and the renderer's other modes, and the copies of that
  thread given to a second one were measured there and none is faster, so
  none of that changed. In 24 to 30 minutes of play in Modern Warfare 2,
  Modern Warfare 3 and Advanced Warfare the clocks were never held back and
  nothing reset.
- **`MW2_PRESENT_AT_BLANK=1`: a frame on a 120 Hz screen 10 ms sooner, for
  whoever would rather have that than every frame** (Android). The app
  hands the screen one frame at each blank the system announces instead of
  keeping the screen's queue full: 23.5 ms from the graphics chip to the
  screen for 33 on the AYN Thor in light scenes. It is off unless asked
  for, because the full queue is also what covers a frame that takes a
  little too long: Advanced Warfare's Traffic ran at 117.5 to 118.9 frames
  a second with it and at 119.1 without. `MW2_PRESENT_MODE` asks for
  another present mode, to measure with.
- **Advanced Warfare: your own map packs and Exo Zombies.** The packs you
  bought on your Xbox 360 (Havoc, Ascendance, Supremacy, Reckoning, Atlas
  Gorge), with the free Compatibility Packs and the game's title update, are
  read from your own copies off the console's hard disk
  (`Content\0000000000000000\41560914`): the maps are under **Bonus Maps**
  in a private match or System Link, and **Exo Zombies** has its own entry
  in the launcher. The project ships none of it and downloads none. The
  packs are for the updated game, so the multiplayer is built a second time
  from your disc and your update: `setup\setup.cmd -Game aw -Dlc <folder>`
  does that and copies the packages to the phone; the app's **Add DLC**
  copies packages from a folder or the Files app and says what is still
  missing. The updated multiplayer plays with others who have the update,
  as on a console; without the update's package the multiplayer is the
  disc's, as before. Not in it: the campaign with the update, building the
  updated multiplayer's code on the device, `setup.sh` on Linux. With an
  update made for a disc in another language than English (the one tested
  is Latin American Spanish) some of the later packs' descriptions and
  menu labels stay in that language (854 strings, most of them the supply
  drops' gear). Seen on Windows and on the AYN Thor (a pack's map with
  bots, a match against a PC through the project's server and Exo Zombies,
  all at 119 FPS; Exo Zombies' later rounds only with test tools) and on
  the Retroid Pocket Flip 2 (a pack's map against a PC, 60 FPS; Exo Zombies
  not run there)
  ([docs/dlc.md](docs/dlc.md#advanced-warfare-map-packs-and-exo-zombies),
  [docs/AW-BRINGUP.md](docs/AW-BRINGUP.md#map-packs-and-exo-zombies-25-november-2026)).
- `MW2_DLC_DIR` takes several folders, with semicolons between.
- **Call of Duty: Advanced Warfare is published**, as the fourth port, in a
  repository of its own: [aw-recomp](https://github.com/yassinsolim/aw-recomp),
  v0.1.0 (`com.aw.recomp`, the campaign, the multiplayer and Exo Survival
  in one app; `README-AW.md` is its front page, with what was measured and
  what was not). You need both discs as `.iso`: the game disc and the
  install disc. The app builds the game code on the device, asks for the
  install disc after the game disc, and updates itself from that
  repository; `setup\setup.cmd -Game aw` and `setup/setup.sh --game aw`
  now download the published app instead of building one
  (`.github/workflows/android-release-aw.yml`,
  [docs/android-releases.md](docs/android-releases.md)). Known limits, as
  published: no level has been played from start to finish by a person,
  Exo Survival's later waves were reached only with test tools, and the
  campaign saves at every checkpoint where the console did not.
- **A frame reaches the screen sooner**
  ([docs/rendering.md](docs/rendering.md#presenting)), all four games on
  Android. No phone's driver here says when a present is on screen, so the
  game's 60 Hz blank ran on a timer beside the display's, and whenever the
  timer was the faster the renderer ran three frames ahead of the screen;
  at an FPS limit of 30 it always did. The blank now follows the display
  by the times Android's own swapchain keeps (`VK_GOOGLE_display_timing`),
  and at 30 a frame waits for the window to have taken the one before it.
  From the renderer finishing a frame to the frame on screen, Retroid
  Pocket Flip 2: 158 ms to 58 ms at 30, 48 ms to 31 ms at 60, 22.5 ms at
  45 as before; AYN Thor 42, 16 and 16 ms. `MW2_REPORT` says it,
  `MW2_REPORT_SECONDS` for each second with the frame's draws and copies,
  and `MW2_FOLLOW_DISPLAY=0` is the timer again.
- **Fewer drops to 45 frames a second on a Snapdragon 865**
  ([docs/android.md](docs/android.md#the-campaigns-slow-places-24-november-2026)),
  in all four games' campaigns on the Retroid Pocket Flip 2:
  - The 3D scene is drawn in fewer pixels for as long as the graphics chip
    is late with a frame, and at its whole size again afterwards
    (`MW2_SCENE_DYNAMIC`, on for an Adreno 6xx; the HUD and the menus keep
    their pixels). Modern Warfare 2's flight to the Gulag: its worst seconds
    46 frames a second to 54, 67% of its frames on time to 87%. Modern
    Warfare 3's Iron Lady: 40 to 50 after each shell to a steady 60, 116
    late frames in two minutes to 2.
  - The first time a level shows a new view its shaders are compiled on
    every core, not on the three the game itself runs on: Modern Warfare
    3's Mind the Gap ran at 41 to 45 for five seconds there and holds 56 to
    60; Advanced Warfare's Traffic opened at 23 to 49 and opens at 50 to 56.
  - The report's time on the graphics chip read nothing for a minute at a
    stretch after the chip's clock had been put back; it is counted again.
    `MW2_REPORT_SECONDS=1` writes a line for each second of play, to find a
    level's slow places; `MW2_SCENE_SHARE` draws the scene at one size, or
    at two in turn, to compare the pictures.
  The AYN Thor runs as it did (S.S.D.D., Mind the Gap and F.N.G. at 118 to
  119). Not changed: Call of Duty 4's Charlie Don't Surf, which is held by
  the renderer's thread where the helicopters land.
- **Advanced Warfare on a phone's efficiency cores**
  ([docs/AW-BRINGUP.md](docs/AW-BRINGUP.md#frame-rate-on-the-two-devices-24-november-2026)):
  the title keeps six threads busy at once and all were held to the
  performance cores, four on an AYN Thor, which were 97% busy in Fusion
  while the other three idled. Its job workers may now run on the
  efficiency cores too (`T_FRAME_PROCESSORS`): 107 frames a second to 115
  there. `MW2_REPORT` says how long each thread stood ready to run with no
  core, and `MW2_TRACE_PACING_FROM` starts the pacing trace late.
- **Fewer clears on a tiling GPU**: the EDRAM clear idiom clears a depth
  target only when something was drawn into it since its last clear, and a
  draw under a scissor at its reset value no longer grows its targets to
  the limit (Advanced Warfare on the Retroid Pocket Flip 2: 13.2 ms of GPU
  a frame to 12.8). `MW2_TIME_PASSES` lists the idiom's clears.
- **Sharpening, and what else was tried of the newer graphics techniques**
  ([docs/rendering.md](docs/rendering.md#sharpening-on-the-way-to-the-window)):
  a Sharpening setting in every title's app (Settings, Graphics; off by
  default) draws the frame to the screen through FSR 1's RCAS filter
  instead of a linear copy, at the screen's full size (`MW2_SHARPEN=<1 to
  100>`, every build). Against the same views drawn at twice the size it
  is 1.8 to 2.9 dB nearer than the app's default path at full strength,
  for 0.35 ms (Retroid Pocket Flip 2) to 0.5 ms (AYN Thor) more GPU a
  frame; the Thor gives up three frames a second in 84 where its GPU is
  the limit. Snapdragon GSR 1 and FSR 1's EASU were built, measured and
  not kept (no nearer than RCAS, the second at more than twice the cost),
  and so was frame generation from two frames (a frame 17 to 18 ms later
  on screen, the HUD smeared where the world moves behind it); the
  temporal upscalers, variable-rate shading and Qualcomm's frame
  extrapolation are set out there with what each would need.
- **Settings changed in the game, in all four titles**
  ([docs/android.md](docs/android.md#the-menu-over-the-game-gamemenukt),
  [docs/ANDROID-PROGRESS.md](docs/ANDROID-PROGRESS.md#the-menu-over-the-game-24-november-2026)):
  the app's menu over a running game is a panel of its own beside the
  picture, worked by touch or by a controller (Select and Start together
  open it), with a Graphics page: display resolution, FPS limit, sun
  shadows, sharpening, field of view, detail at distance (Modern Warfare 2), texture
  memory, the frame-rate line and Fill the screen change as they are
  chosen, without leaving the level; the internal render multiplier and
  multisampling are marked as taking effect at the next start. Every
  choice is the launcher's Settings' own (`Prefs`). Behind it the runtime
  takes `MW2_FPS_LIMIT`, `MW2_SHADOWS`, `MW2_FOV`, `MW2_LOD_BIAS` and
  `MW2_TEXTURE_BUDGET_MB` under a running title
  (`NativeBridge.nativeApplyOption`, `runtime/live_options.h`). Quit asks
  on a page a controller can answer, and the bots are a row.
- **Advanced Warfare: Exo Survival's Find Match and Combat Readiness
  Program on a relay**
  ([docs/AW-BRINGUP.md](docs/AW-BRINGUP.md#exo-survival-under-xbox-live-combat-readiness-program-24-november-2026-windows)).
  The playlists now have the title's categories
  (`relay/playlists_aw.info`): Exo Survival's Find Match lists Exo
  Survival, where it listed Team Deathmatch and Free-for-all and opened a
  Team Deathmatch lobby, and two copies on Windows meet by it and play
  round 1; Combat Readiness Program starts a Team Deathmatch with one
  player and the host's bots. The multiplayer's Find Match reads as
  before. Seen on Windows only.
- `setup\setup.ps1` writes Advanced Warfare's Windows settings (`.env`)
  with the relay and `MW2_UNLOCK_ALL=1`, as Modern Warfare 3's.
- **Advanced Warfare's checkpoints are kept**
  ([docs/AW-BRINGUP.md](docs/AW-BRINGUP.md#checkpoints-the-left-trigger-setupsh-24-november-2026),
  [docs/saves.md](docs/saves.md#advanced-warfares-checkpoints)): the title
  writes a checkpoint to the save only at the pause menu's Save and Quit,
  so a campaign left any other way resumed at the level's start. The
  runtime now gives the title's own `savegame_lastcommit` whenever a
  checkpoint is committed; Resume Game after the app was stopped goes on
  from the checkpoint, on Windows and on the Retroid Pocket Flip 2, in a
  release-signed build too. `MW2_NO_CHECKPOINT_WRITE=1` keeps the
  console's way. `setup.sh` has been run from end to end for this title
  (5 min 46 s on Ubuntu under WSL2, installed on the Flip 2).
  `MW2_INPUT_LIVE=<file>` (diagnostic) is an input script written while
  the game runs. Still open: a level played from start to end without a
  start point. Fixed with it: a tap on the middle of the touch LT pressed
  the D-pad on the Flip 2 (where touch controls overlap, the touch is the
  nearer one's, in every title's app); `setup.sh` pushed one of Advanced
  Warfare's two packages; and `setup.sh` stopped at libadrenotools when
  run a second time, for every title.
- **Advanced Warfare's multiplayer on the two devices, the rest**
  ([docs/AW-BRINGUP.md](docs/AW-BRINGUP.md#the-rest-on-the-devices-24-november-2026)):
  the AYN Thor and the Retroid Pocket Flip 2 meet by Xbox LIVE's Find
  Match through the relay (119 and 60 frames a second); Exo Survival
  starts from the campaign's own menu entry on both, and the two play it
  together through the relay; a bot leaves a full match for a joining
  player. Exo Survival's rounds 2 to 5 and its Exo upgrade station were
  seen with a test harness that keeps the player alive, which is not in
  the source: the title has no cheat for it, and without one the players
  driven here were killed in round 1.
- **Advanced Warfare's frame rate, measured on both devices**
  ([docs/AW-BRINGUP.md](docs/AW-BRINGUP.md#frame-rate-on-the-two-devices-24-november-2026)):
  `MW2_SCENE` for both of its executables, and an 832x480 scene on an Adreno
  6xx as in the other titles (Seoul's drop pod on the Retroid Pocket Flip 2:
  57.8 frames a second at the console's scene, 59.5 with it); at 60 on a
  60 Hz display the display paces it, not the guest's blank (79% of frames
  on time to 95%). On the AYN Thor the campaign's first level and a bot
  match hold 118 to 119 at the 120 cap, without the sun's shadows as
  Modern Warfare 3; its heavier Fusion is 108.
- **Advanced Warfare's campaign, a level's end and the Flip 2**
  ([docs/AW-BRINGUP.md](docs/AW-BRINGUP.md#a-levels-end-the-next-level-the-flip-2-24-november-2026)):
  on Windows Seoul's ending plays into its After Action Report and on into
  Atlas, from a start point of the level's own script (`set start
  seoul_finale_scene_start` before `devmap seoul`), and Atlas goes on to
  its training exercise. The Retroid Pocket Flip 2 plays New Game into
  Seoul, Resume Game and Atlas from Mission Select at 60 frames a second,
  from an app that one unattended `setup.ps1 -Device` run built and
  installed (8 min 43 s from the two disc images), and from game code it
  built itself (12 min 41 s, [docs/ondevice-build.md](docs/ondevice-build.md)).
  Known: a checkpoint within a level is reached and reloaded after a
  failure, and is not written to the save, so Resume Game starts the level
  again.
- **Call of Duty: Advanced Warfare, started**
  ([docs/AW-BRINGUP.md](docs/AW-BRINGUP.md)): titles `aw` and `awmp`, for
  Windows and Android (`com.aw.recomp`, built by `setup\setup.cmd -Game aw
  -Iso <game disc> -InstallIso <install disc>`; nothing is published). Both
  executables recompile clean, the install disc's two content packages are
  mounted as the game expects, and the campaign reaches its main menu and
  plays its first level on Windows and on the AYN Thor, with its exposure
  right (the scene's luminance is written back for it). The app's installer
  takes the two disc images one after the other. A first run's Brightness
  and Safe Area screens want an adjustment (a direction) before they take A.
  `tools/check_aw_xex.py`, `tools/extra_instructions.py`,
  `tools/ctr_branch_extents.py`.
- **Advanced Warfare's stepped-over instructions, checked.** The twelve
  instructions and the two half-float unpacks its configuration replaces by
  calls (`runtime/recomp/ppc_extra_instructions.cpp`) are compared with
  their definitions, element order included, by
  `tests/ppc_extra_instructions_test.cpp`: cases worked out by hand, every
  half float there is, and random sweeps against a model written in the
  console's terms. All fourteen are as defined (run on x86-64, with and
  without optimisation); none needed a change.
- **Advanced Warfare's Xbox LIVE menu, on a relay.** Its logon to the
  publisher's servers (one HTTPS request through XHttp, answered inside the
  game), the lobby, the playlists (`relay/playlists_aw.info`: Team
  Deathmatch and Free-for-all on the disc's thirteen maps) and its
  matchmaking, which lists and finds sessions through the relay's listings.
  Seen with two copies on Windows through `mw3.yassin.app`: Find Match into
  one match, and a private match joined by invitation. The relay needs no
  change. `MW2_UNLOCK_ALL` answers its item check again, which applies
  under Live. Exo Survival starts from launch data (the mode the campaign's
  menu hands over) and was played by one copy and by two through the relay,
  round 1 only. Nothing of this was run on a device
  ([docs/AW-BRINGUP.md](docs/AW-BRINGUP.md)).
- `setup/setup.sh` builds Advanced Warfare on Linux (`--game aw`; with
  `--install-iso` the install disc's two packages are read out of its image
  and copied to the phone).
- `MW2_AUDIO_DUMP=<file.wav>` writes the title's mix as it submits it, for
  checking the sound of a run nobody listened to
  ([docs/switches.md](docs/switches.md)).
- **Advanced Warfare's multiplayer plays a System Link match on Windows**
  ([docs/AW-BRINGUP.md](docs/AW-BRINGUP.md#the-multiplayer)): hosted from
  Create Game on Ascend, Bio Lab, Detroit and Retreat, and joined by a second
  copy over the local network. `MW2_BOTS`, `MW2_BOT_SKILL`, `MW2_BOT_TEAMS`
  and the console's `bots` line set the title's own bots (11 at most), which
  use the maps' paths and the exo's jumps. The title has a network link when
  a service started and its sockets are the service's, as Modern Warfare
  3's. Its streamed textures are no longer noise (`T_Image_FlushMove`).
  Create a Class needs no unlock switch in System Link: a new profile's
  Pick 13 is full and its weapons have no padlock. On the AYN Thor and the
  Retroid Pocket Flip 2 a hosted match with 6 bots holds 119 and 60 frames a
  second (Retreat, Detroit, Comeback), and the two play one match through
  the relay, each hosting in turn. Not working yet: Xbox LIVE.
- **Advanced Warfare: a join is no longer lost while the level loads.** A
  copy joining a match says nothing while its first frames of the level
  compile their shaders (6 to 11.5 seconds on Windows), and the title's
  host drops a player after 10 (`sv_timeout`): "Server Disconnected", on
  the local network as through the relay, by chance. A host built from this
  source waits as long as the title waits for a player still connecting, 40
  seconds (`runtime/aw_server.cpp`). The relay is unchanged. A copy silent
  for twenty seconds or more still gives up by itself.

### Fixed
- **Advanced Warfare: the sun's far shadows are there in every frame.** With
  Sun shadows on, shadows a few metres from the player and beyond, a
  building's on the ground, came and went for single frames while standing
  still (a tester's "yellow wedge" on Solar; Riot the same). The title
  writes the nearest depth into the middle of the far shadow map, the part
  the near map covers, once before the map's casters and once after the
  command buffer a worker recorded for them has run; the renderer answered
  either by emptying every depth image, so the map held only the casters
  drawn after the second: none when the worker had finished in time, some
  or all when the title had to draw them itself. Such a rectangle is now
  left undrawn (`runtime/gpu/vulkan/renderer_draw.cpp`,
  [rendering.md](docs/rendering.md)). The app only; the game code is as it
  was.
- **An invitation goes only to the player you choose.** "Invite network
  players" in the pause menu, the lobby's Invite and F6 on Windows invited
  everyone the game could hear, and through the server that is everybody
  playing there: a stranger's copy got one in testing. Now you get a list
  of the players heard (**Invite a network player** in the app, the
  friends screen on Windows) and the one you pick is the only one invited.
  Through the server a private match is also no longer announced to
  everyone: only the players you invited are told where it is, so nobody
  else can list it under "Join network game" or join it. On your own
  network it is still shown to everyone there. A friend on an older
  version can still be invited; to join you from his pause menu without an
  invitation he needs this one. Checked on a Retroid Pocket Flip 2 through
  `mw3.yassin.app`, with two copies on a PC set to accept invitations and
  somebody else's copy on the server: the one tapped joined the lobby and
  the match, the other two heard nothing
  ([docs/multiplayer.md](docs/multiplayer.md#lan-and-server)). The server
  itself is unchanged.
- **Advanced Warfare: Combat Readiness Program has its bots.** The match
  started with you alone in it and was forfeited after twenty seconds. The
  mode is the game's practice match, three bots a side, and it fills when
  the playlist says so; ours did not (`relay/playlists_aw.info`). On a
  Flip 2 the match now plays at 60 FPS beside and against bots. The app's
  **Bots**, when not at None, still sets how many
  ([docs/AW-BRINGUP.md](docs/AW-BRINGUP.md#exo-survival-under-xbox-live-combat-readiness-program-24-november-2026-windows)).
- **System Link between two devices that do not find each other: the app
  says why.** Our two test devices stopped hearing each other over LAN,
  and the cause was the network, not the game: one was on the router's
  Wi-Fi and the other on a second router's (a mesh system with a network
  of its own). LAN only reaches devices on the same Wi-Fi network. With
  nobody heard, "Join network game" and "Invite a network player" now ask
  whether both devices are on the same Wi-Fi and point to **NETWORK:
  Online**, which joins two networks; the READMEs' troubleshooting and
  [docs/multiplayer.md](docs/multiplayer.md#lan-and-server) say the same.
  On one network LAN works as before: an AYN Thor listed and joined a
  PC's System Link match (119 FPS).
- **A rare freeze for good in the middle of play, in all four games.** The
  runtime watches the memory the game hands the GPU by making its pages
  read-only, and a game thread that writes to one makes it writable again.
  A thread put aside in the middle of that, while another thread wrote to
  the page and the renderer watched it again, could leave the page
  read-only and recorded as writable: every later write to it then failed
  and was tried again for ever, with the picture stopped. The next write to
  such a page now puts it right (`runtime/gpu/memory_watch.cpp`). It was
  found in a title still being brought up, which stopped there in two
  matches of six and in none of the thirteen played since. No freeze of the
  four published games has been traced to it, but they run the same code
  and could have met it. App only: no game code is built again. On an AYN
  Thor, Modern Warfare 2 (S.S.D.D.) drew the same frames with and without
  it at the same speed, Call of Duty 4 (The Bog) and Advanced Warfare
  (Lagos) played, and Modern Warfare 3 was run in its menus only.
- **Advanced Warfare at 2x and 3x: the picture in a level was a white
  wash.** With **Settings → Internal render multiplier** above 1x
  (`MW2_SCALE`) the menus and the lobby were right and a match was all but
  white, on the AYN Thor and on Windows. The title works its exposure out
  from a 16x16 surface of the scene's luminance that it reads from memory,
  and the write-back that gives it one was left out under a scale: the
  title took every scene for pitch dark. The surface is now read at its
  drawn size and each texel written as the mean of the four or nine drawn
  for it ([docs/rendering.md](docs/rendering.md#resolution-scale)).
- **Add DLC said "No map packs here yet" for the whole of a copy** from a
  folder chosen with the system's picker (the screen's resume wrote over
  "Copying…"): a quarter of an hour of it with a console's whole folder.
- **`setup.sh` and Advanced Warfare's two discs.** Given the install disc
  as `--iso`, setup refused its `default.xex` as "Unsupported" and left it
  in `aw/tu0`, where the next run found it and refused the right disc too.
  A refused executable is now deleted, the two images given the wrong way
  round are taken the right way round, and the message otherwise says which
  option takes which disc.
- **`setup.sh` and the system's programs.** The programs it needs are
  checked before anything else and named with the one command that
  installs them (apt, dnf, pacman or zypper). It offers to run that
  command only at a keyboard: with `--yes` it used to start `sudo` and wait
  for a password nobody was there to type. A failing `apt-get update` (a
  third-party repository's expired key) no longer stops it.
- **`setup.sh --xex-dir` naming the repository's own folder** (`aw/tu0`)
  stopped at `cp: ... are the same file`.
- **The sound no longer falls behind the picture the longer you play, in all
  four games on Android.** The mixer made its 256-sample frames every 5333
  microseconds on the system's clock, 62 samples in a million more than a
  device plays, and what was made beyond what was played stayed in the
  output's queue for good: 3.3 ms more every minute on the Retroid Pocket
  Flip 2 and 3.7 on the AYN Thor (the queue from 4 to 41 ms in ten minutes
  there), so the sound of everything was a tenth of a second late after half
  an hour and 170 ms late, where the queue is full, after about fifty
  minutes. A cutscene's speech against its lips is where it shows, and a
  Bink movie's too: the title times a movie's pictures to the sound it has
  handed over, not to what is heard. Now the mixer follows the device (half
  a hundredth slower while the queue holds more than it does when both are
  on time, as much faster while it is empty), and the queue is 6.7 ms after
  ten minutes as at the start there, and on the Flip 2 a movie started ten
  minutes in has its sound 17 ms after its picture (12 ms for one started
  after thirty seconds). A mixer frame that came late (a level
  starting, pipelines compiling) used to be passed over, which left every
  sound then playing that much behind until it ended (107 ms in one level
  start on the Thor, 60 ms over two minutes of a loading movie on the Flip
  2): it is mixed now, and the frames mixed late are not played once the
  queue is back to what it holds on time. Measured with `MW2_TRACE_MOVIES=1`
  (new: a movie's pictures, the mixer's frames and the output's queue on one
  clock, [docs/switches.md](docs/switches.md)) against the movie's own sound
  track found in `MW2_AUDIO_DUMP`'s file; [docs/audio.md](docs/audio.md).
  An application update, with nothing to build again.
- **The log says when the device keeps the sound off its low-latency
  output.** A system-wide equaliser or sound effect does that, and so do
  Bluetooth headphones; the sound is then a tenth of a second and more
  behind the picture whatever the game does (a movie's sound 124 ms after
  its picture on the AYN Thor with JamesDSP on, 36 ms with it off).
- **A folder of disc images is taken as that, in all four games.** A player
  who keeps his `.iso` files in a folder of their own
  (`<SD card>/Roms/xbox 360/Call Of Duty - Advanced Warfare/`) and chose that
  folder was treated as having chosen a disc's extracted files: the build
  screen said there was no `default.xex` in it, and the install screen
  switched to **Copy the game files into the app** and failed the same way.
  Now the images in a chosen folder are told by what is in them (the title's
  number in the executable, Advanced Warfare's install disc by its package
  folder) and taken: both of Advanced Warfare's at once. The browser says
  that an `.iso` can be tapped, a folder with nothing of this game says so,
  and when a folder really is extracted files the install screen says why
  **Play from the disc image** is greyed out instead of only switching.
- **A disc image is found wherever it lies.** The build screen and the
  launcher looked in the Download folders only; they now look through the
  device's storage and each memory card, four folders deep, for an image of
  this game under whatever name. Advanced Warfare's install disc is found
  with its game disc, and the two can be selected together in the Files app.
- The app's browser lost the way back to the device's own storage once a
  memory card had been opened; the top of each storage now lists the others.
- **Advanced Warfare no longer flickers.** A few times a second one frame
  was brighter than its neighbours, the whole picture with the HUD (seven
  times in eight seconds of the multiplayer's lobby and ten in twelve
  seconds of a match on a Retroid Pocket Flip 2; the logs of Windows runs
  show it too). The game's Brightness reached the screen for those
  frames only: the rest were shown through a plain ramp, because a block
  the game's graphics library reads at every frame was left as the stack
  had it. It is filled now, so every frame has the game's Brightness and
  the picture is a little brighter than before; lower
  **Options > Brightness** if you had raised it to make up. Modern Warfare
  3 never showed its own Brightness for the same reason and does now;
  Modern Warfare 2 and Call of Duty 4 are unchanged
  ([docs/rendering.md](docs/rendering.md#gamma)).
- **Call of Duty 4 holds an FPS limit of 45**, set in the game's menu or
  before the start: its own limiter let three frames through in the time
  of one (136 a second, which the display then paced), so the runtime
  holds its presents to the limit instead: 45.0 on both devices.
- **The on-screen controls stay off** (and every other choice made in the
  game stays made): the launcher and the game are two processes with one
  preference file, each wrote its own stale copy over the other's, and a
  write still queued when the game's process was killed was lost. The
  file is read again when the other process has written it, and the game
  waits for its writes before it ends. With them off, the Menu button and
  Select and Start on a controller still open the menu that turns them on.
- Settings offered Detail at distance in Call of Duty 4 and Advanced
  Warfare, which have no such hook; it is Modern Warfare 2's alone.
- **Advanced Warfare: wall panels drawn as coloured noise for a whole
  match, now and then.** The title draws some moves of its streamed
  images' pool in pieces; the copy was made at the first piece and the
  move forgotten, so a later piece could be taken for the next move to
  the same place, whose copy was then made too early, from memory the
  pool had not filled yet (4 to 146 times in 150 seconds on each of the
  thirteen multiplayer maps; seen as noise once, on Bio Lab's outside in
  Exo Survival). A move is now held until its last piece
  (`runtime/image_move.cpp`,
  [docs/AW-BRINGUP.md](docs/AW-BRINGUP.md#wall-panels-as-coloured-noise-24-november-2026)).
  Modern Warfare 2 and 3 draw every move in one piece and are unchanged.
- **Advanced Warfare: Create a Class had padlocks under Xbox LIVE on a
  phone.** The app set `MW2_UNLOCK_ALL` for Modern Warfare 3 only; it now
  sets it for Advanced Warfare's multiplayer too, with its own switch in
  Settings ("Unlock all items", on unless turned off).
- `NtCreateSemaphore` read its counts from the registers of Windows' call,
  which has an access mask this console's has not (Modern Warfare 3 imports
  it too).
- **Advanced Warfare: textures drawn as coloured noise.** The title's pool
  of streamed images moves a block by having the GPU copy it, which the
  renderer does not do; the copy is now made as it is for the earlier
  titles (`T_Image_FlushMove`, `runtime/image_move.cpp`). Buildings, faces
  and the drop pod of the first level were wrong from its first minute.
- **Setup no longer installs on a phone nobody chose.** With more than one
  phone connected, a run that asks nothing (`-Yes` / `--yes`, or no
  keyboard) and names none (`-Device` / `--device`) took whichever adb
  listed first, and for Advanced Warfare copied 4 GB to it. It now installs
  on none, says the build is ready and prints the command for each phone.
  With somebody at the keyboard, or one phone, nothing changes
  (`setup/setup.ps1`, `setup/setup.sh`).

## [v0.1.7] - 2026-11-24

Modern Warfare 3's and Modern Warfare 2's v0.1.7 and Call of Duty 4's
v0.1.2, from the same commit.

### Highlights
Each game's release notes, in plain text.

Modern Warfare 3, v0.1.7

- No PC needed: build the game code on your device in about 5 to 8 minutes.
  Install the app, choose Build on this device, pick your disc image (.iso)
  with the Files app, and tap Build. The app fetches the compiler once (43
  MB), then works in the background with a progress bar: you can pause it,
  use other apps or lock the screen. Measured: 4 min 40 s on an AYN Thor
  (Snapdragon 8 Gen 2), 8 min on a Retroid Pocket Flip 2 (Snapdragon 865, 8
  GB). A PC still works as before (setup.cmd, or setup.sh on Linux).
- Play straight from the disc image. Install & Play no longer copies 7 GB:
  the game reads its files out of the .iso where it lies, on the device or
  on an SD card, and is ready in a second. Loading takes the same time as
  from copied files. Copying is still a choice, and Settings, Files moves an
  installed game either way.
- The image is chosen once: Install & Play uses the one you picked for the
  build.
- Your own map packs (experimental): the multiplayer lists and mounts the
  downloadable content packages you copy from your own console into a dlc
  folder. Tried with test packages only; no real map pack yet.
- The start screen names the two steps of setting up, says which is done,
  and leads to the first.
- A hot device or one short of memory builds with fewer compilers instead of
  stopping; a build Android interrupts carries on by itself.

The game package built for v0.1.0 to v0.1.6 still fits: nothing to build
again.

Modern Warfare 2, v0.1.7

- No PC needed: build the game code on your device in about 3 to 6 minutes.
  Install the app, choose Build on this device, pick your disc image (.iso)
  with the Files app, and tap Build. The app fetches the compiler once (43
  MB), then works in the background with a progress bar: you can pause it,
  use other apps or lock the screen. Measured: 3 min on an AYN Thor
  (Snapdragon 8 Gen 2), 5 min 30 s on a Retroid Pocket Flip 2 (Snapdragon
  865, 8 GB). A PC still works as before (setup.cmd, or setup.sh on Linux).
- Play straight from the disc image. Install & Play no longer copies 7 GB:
  the game reads its files out of the .iso where it lies, on the device or
  on an SD card, and is ready in a second. Loading takes the same time as
  from copied files. Copying is still a choice, and Settings, Files moves an
  installed game either way.
- The image is chosen once: Install & Play uses the one you picked for the
  build.
- Your own map packs (experimental): the multiplayer lists and mounts the
  downloadable content packages you copy from your own console into a dlc
  folder. A test package played Rust; no real map pack has been tried yet.
- The start screen names the two steps of setting up, says which is done,
  and leads to the first.
- A hot device or one short of memory builds with fewer compilers instead of
  stopping; a build Android interrupts carries on by itself.

The game package built for v0.1.0 to v0.1.6 still fits: nothing to build
again.

Call of Duty 4, v0.1.2

- No PC needed: build the game code on your device in about 2 to 4 minutes.
  Install the app, choose Build on this device, pick your disc image (.iso)
  with the Files app, and tap Build. The app fetches the compiler once (43
  MB), then works in the background with a progress bar: you can pause it,
  use other apps or lock the screen. Measured: 2 min 12 s on an AYN Thor
  (Snapdragon 8 Gen 2), 3 min 49 s on a Retroid Pocket Flip 2 (Snapdragon
  865, 8 GB). A PC still works as before (setup.cmd, or setup.sh on Linux).
- Play straight from the disc image. Install & Play no longer copies 7 GB:
  the game reads its files out of the .iso where it lies, on the device or
  on an SD card, and is ready in a second. Loading takes the same time as
  from copied files. Copying is still a choice, and Settings, Files moves an
  installed game either way.
- The image is chosen once: Install & Play uses the one you picked for the
  build.
- Your own map packs (experimental): the multiplayer lists and mounts the
  downloadable content packages you copy from your own console into a dlc
  folder. Tried with test packages only; no real map pack yet.
- The start screen names the two steps of setting up, says which is done,
  and leads to the first.
- A hot device or one short of memory builds with fewer compilers instead of
  stopping; a build Android interrupts carries on by itself.

The game package built for v0.1.0 and v0.1.1 still fits: nothing to build
again.

### Added
- **Build the game code on the device itself**
  ([docs/ondevice-build.md](docs/ondevice-build.md)): no PC and no GitHub,
  for all three games. The launcher's two steps lead with **Build on this
  device**; the build screen takes the disc's two executables from a disc
  image chosen with the Files app (on the device or a memory card, no
  storage permission), from one in Download or from the installed game
  files, installs the toolchain package from its release, and recompiles,
  compiles and links in the background with a progress bar, the time left,
  pause and carry on. Whole builds: Modern Warfare 2 in 3 minutes on an AYN
  Thor and 5 and a half on a Retroid Pocket Flip 2 (8 GB), Modern Warfare 3
  in 4 min 40 s and 8, Call of Duty 4 in 2 min 12 s and 3 min 49 s. Played
  on what was built: Call of Duty 4's F.N.G. on both devices, and a System
  Link match hosted in each game. The library is loaded from the app's own
  storage; a game package built on a PC still works. After an update that
  needs new game code the same entry builds it again, with no disc image.
  - **The toolchain package has a release of its own**
    ([docs/android-releases.md](docs/android-releases.md#the-toolchain-release)):
    `toolchain.yml` builds `recomp-toolchain.apk` from sources on a GitHub
    runner (45 minutes), signs it with the release key and drafts the
    pre-release `toolchain-v1` in Modern Warfare 3's public repository, with
    the licences, every notice and the recompiler's whole source (it is
    under the GPL-3.0). `setup/toolchain.json` names the release, the
    version and the APK's SHA-256; an app downloads that file, hands it to
    Android's installer only when the hash is the list's, and uses only a
    package of that version. `MW2_TOOLCHAIN_URL` and
    `tools/serve_test_release.py --toolchain` try it before it is published.
  - The disc image is chosen once: the one the build took the executables
    from is what Install & Play offers, a document on a memory card
    included.
  - Memory and heat were made to happen on the Flip 2: with 1.3 GB free the
    build ran seven compilers and then eight again; with 0.4 GB Android
    ended it and it started again by itself and finished; at a reported
    47 °C it ran four, at 51 °C one.
- **Building the game code without a Windows PC**
  ([docs/BUILD-WITHOUT-PC.md](docs/BUILD-WITHOUT-PC.md)).
  - `setup/setup.sh`: setup for Linux, for all three games
    (`bash setup/setup.sh --game mw2 --iso <file>`). It fetches a pinned JDK
    and the Android SDK and NDK into `.toolchain/`, reads and checks the
    executables, recompiles, builds the game package, downloads the
    published app and installs both over adb (`--device`). The number of
    compilers follows the free memory, and it says how much memory and time
    the build took. Android only; the Windows game stays `setup.ps1`'s.
    Tested on Ubuntu 24.04 (WSL2) from a fresh clone for Modern Warfare 2:
    2 minutes 32 seconds, 3.3 GB of memory in use at most, the same
    statement as the Windows build's, and the package starts the game on a
    Retroid Pocket Flip 2. One compiler takes 320 MB at most.
  - With no PC at all: the app's **Settings → Files → Prepare files for
    building online** writes the disc's two executables into Download (from
    the installed game files, or read out of the disc image), and the
    workflow in `setup/recomp-builder/` (the template repository
    `yassinsolim/recomp-builder`) builds the game package from them in the
    player's own private repository. It refuses to run in a public
    repository and keeps the package as a draft release, which only the
    owner can see. Not yet run on GitHub from start to finish, and not in
    the READMEs until it has been.
  - The app's messages about missing or outdated game code name both.
- The build tools for building the game code on the phone, as a package of
  their own (`com.recomp.toolchain`, `android/toolchain`,
  `tools/toolchain/build.sh`): clang and lld 19.1.7, XenonRecomp and
  XenonAnalyse for arm64 Android, with Android's headers from AOSP and LLVM's
  libraries built from source, nothing of the NDK's. Modern Warfare 2's game
  library builds with it on an AYN Thor in a minute and a half and on a
  Retroid Pocket Flip 2 in two and a half, and states what the PC's states
  ([docs/ondevice-build.md](docs/ondevice-build.md)).
- `tools/check_no_game_files.py` refuses a tracked APK and any tracked file
  over 20 MB.
- **Play from the disc image, on the device or a memory card (all three
  games, Android).** Install & Play no longer copies 7 GB: the game reads its
  files out of the `.iso` where it is, and only the two executables and the
  start screen's picture (11 MB) go into the app. The image may be in
  `Download`, on a memory card, or anywhere the Files app reaches (the app
  keeps the permission and reads through the open document, so no all-files
  access is needed). Copying the files into the app is still a choice on the
  install screen, and **Settings → Files → Change where the game is read
  from** moves an installed game either way, offering to delete the 7 GB of
  copies. Saves, settings, mods, camouflage packs and an update's files stay
  in the app, and a file there is read before the image's. Measured on a
  Retroid Pocket Flip 2 (cold file cache, two runs each): Modern Warfare 2's
  S.S.D.D. loads in 3.6 s from the image on the device, 3.7 s from copied
  files and 3.8 s from the image on a memory card (exFAT, 59 MB/s); The Gulag
  in 6.2, 6.3 and 6.9 s; no late frames in the half minute after either.
  From the image: Modern Warfare 2's Terminal (multiplayer) 5.9 to 6.5 s,
  Modern Warfare 3's Survival on Resistance 13.2 to 13.5 s, Call of Duty 4's
  F.N.G. 5.1 s with its high mips read from the image's `highmip` folder
  (`runtime/kernel/volume.{h,cpp}`, `kernel/files.cpp`, `DiscImage.kt`,
  `InstallActivity.kt`; [docs/android.md](docs/android.md#the-game-files),
  [docs/runtime.md](docs/runtime.md#files)).
- **Your own map packs (experimental).** The multiplayer of each game lists,
  mounts and reads the downloadable content packages a player copies from
  their own console into a `dlc` folder, where they lie, with the licence
  bits the package carries. Tested with synthetic packages made from a disc's
  own files (Modern Warfare 2 played Rust out of one); **no real map pack has
  been tried**, and Modern Warfare 2's and 3's were made for a later game
  version than the disc's. What to supply, what the runtime does and what is
  untested: [docs/dlc.md](docs/dlc.md).
- **The two steps of setting up, said first.** The app is published without
  the game's code, and a player who installed it alone ran Install & Play and
  only then read "The game code is not on this device". The launcher now
  shows both steps with which is done (1. build the game code, on the device
  or on a PC; 2. Install & Play), in a dialog at the start while the game
  code is missing, with **Build on this device** and a button to the
  README's new **Two steps to play** section, which each of the three
  READMEs now opens with.
- The file browser lists a memory card at the top of the device's storage.

### Tests
- `kernel_files_test`: a synthetic disc image (by path and by descriptor)
  under the game folder, and a synthetic content package mounted as a drive;
  `kernel_xam_test`: the downloadable content enumerator. Both were built
  with the NDK and run on an arm64 Android device.

### Fixed
- Install & Play offered another game's disc image when it was the only one
  in Download (Modern Warfare 3's, to Modern Warfare 2's app): the image
  the build screen checked comes first.
- "1 compilers at once".

## [v0.1.6] - 2026-11-24

Modern Warfare 3's and Modern Warfare 2's v0.1.6 and Call of Duty 4's
v0.1.1, from the same commit.

### Highlights
Each game's release notes, in plain text.

Modern Warfare 3, v0.1.6

- Private matches and invitations work through the server. Joining a
  private match ended in "Unable to join game session"; the pause menu now
  has Invite network players and Join network game.
- A name of your own online. Every device was "Player 1". The name is now
  your device's (for example "AYN Thor 5DF4") until you type one, and the
  multiplayer asks once at its first start.
- The start screen says when the system holds the screen under the FPS
  limit (an AYN Thor put back to 60 Hz) and how to change it: Settings,
  Display, Smooth Display, or the refresh rate tile in the quick settings.
- Join network game lists every player, two of one name included.
- After a new graphics driver or a lost driver cache, the pipelines of maps
  played before are compiled on the fast cores during menus and loading:
  413 in 5.7 s on a Retroid Pocket Flip 2, where they took 39.9 s.
- A tidier update dialog: the notes as a list, and whole. You see it from
  the next update on.
- The launcher no longer starts the game again after you quit, when Android
  had closed the launcher during the run.
- The bots run on code now shared by all three games; they play as before.

The game package built for v0.1.0 to v0.1.5 still fits: nothing to build
again.

Modern Warfare 2, v0.1.6

- A name of your own online. Every device was "Player 1". The name is now
  your device's (for example "AYN Thor 5DF4") until you type one, and the
  multiplayer asks once at its first start.
- The start screen says when the system holds the screen under the FPS
  limit (an AYN Thor put back to 60 Hz) and how to change it: Settings,
  Display, Smooth Display, or the refresh rate tile in the quick settings.
- Join network game lists every player, two of one name included.
- A tidier update dialog: the notes as a list, and whole. You see it from
  the next update on.
- Small CPU savings in the renderer: its look-ups went from 9% of the
  render thread's time to 5% in S.S.D.D. The frame rate is the same within
  what two runs differ by.
- After a new graphics driver or a lost driver cache, the pipelines of maps
  played before are compiled on the fast cores during menus and loading.
- The launcher no longer starts the game again after you quit, when Android
  had closed the launcher during the run.
- The bots run on code now shared by all three games; they play as before.

The game package built for v0.1.0 to v0.1.5 still fits: nothing to build
again.

Call of Duty 4, v0.1.1

- Bots. On the start screen: Multiplayer, then BOTS, to choose how many (up
  to 17) and how good. Host a System Link match and they join it, pick a
  team and a class, fight, score, and leave to make room when a player
  joins. The pause menu changes their number in a match. Six on Crash
  played a ten-minute match at 60 FPS on a Retroid Pocket Flip 2.
- Their limits: the maps have no path data for them, so they feel their way
  round walls; no grenades or objectives. They join only the matches your
  device hosts.
- Xbox LIVE Find Match in Free-for-all, and a private match joined from the
  pause menu (Join network game), were played between two devices through
  the server.
- Quitting after INSTALL & PLAY stays in the launcher: it started the game
  again when Android had closed the launcher during the run.
- A name of your own online. Every device was "Player 1". The name is now
  your device's (for example "AYN Thor 5DF4") until you type one, and the
  multiplayer asks once at its first start.
- The start screen says when the system holds the screen under the FPS
  limit (an AYN Thor put back to 60 Hz) and how to change it: Settings,
  Display, Smooth Display, or the refresh rate tile in the quick settings.
- A tidier update dialog: the notes as a list, and whole. You see it from
  the next update on.

The game package built for v0.1.0 still fits: nothing to build again.

### Added

- **Bots in Call of Duty 4's multiplayer.** The launcher's **Bots** entry,
  Settings → Multiplayer and the pause menu's **Bots in my matches** are in
  Call of Duty 4's app as well: choose how many (up to 17) and how good,
  host a System Link game, and the bots join it, pick a team and a class,
  fight, score and leave to make room when a player joins. On the PC:
  `MW2_BOTS` (up to 23: this game's server has 24 slots), `MW2_BOT_SKILL`,
  `MW2_BOT_TEAMS`. `runtime/bots.cpp` plays all three games' bots now; Call
  of Duty 4's addresses are in `runtime/title.h` with how each was found
  ([docs/multiplayer.md](docs/multiplayer.md#call-of-duty-4s-bots)). The game
  package built for v0.1.0 still fits: nothing to build again.
- **A name of your own online.** Every device was "Player 1". The name is now
  the device's until you give one (its model and four characters, "AYN Thor
  5DF4"), and the first start of the multiplayer asks for it. On Windows
  setup offers the computer's name the same way.
- **The launcher says when the screen is held under the FPS limit** (an AYN
  Thor put back to 60 Hz), and where to change it; the note in the game fits
  the two lines Android shows of it.
- **Modern Warfare 3: Invite network players and Join network game** in the
  pause menu whenever it plays through a relay.

### Fixed
- **Join network game lists every player.** Two players with the same name
  were one line; they are told apart by their ids now.
- **Modern Warfare 3's private match can be joined** (and an invitation to it
  accepted). It ended in "Unable to join game session" after twenty seconds.
- **The update's notes** are shown as a list with titles, without Markdown's
  signs, and whole.
- **The launcher does not start the game again** after a Quit when Android
  had ended the launcher under the run.
- **After a new graphics driver or a lost driver cache** the pipelines a
  previous run recorded are compiled on the fast cores while a menu or a
  loading screen is up: 413 of them in 5.7 s on a Retroid Pocket Flip 2
  where they took 39.9 s.

### Changed
- **The ring consumer's look-ups, measured on the AYN Thor**
  ([ANDROID-PROGRESS.md](docs/ANDROID-PROGRESS.md#the-ring-consumer-profiled-on-the-ayn-thor-24-november-2026)).
  The vertex ranges a frame slot keeps are two rows in the order of their
  starts in place of trees, and the ranges met once a table; the shaders,
  pipelines and shader loads asked for lately are found by their hash before
  the maps are; an address' heap block is remembered by its megabyte. By
  simpleperf the look-ups go from 9% of the consumer's time to 5% in Modern
  Warfare 2's S.S.D.D.; the frame rate is what it was within what two runs
  differ by (Team Player 82.2 to 83.9 frames a second before, 81.5 to 85.7
  after), and the picture is the same.

## [v0.1.5] - 2026-11-23

Modern Warfare 3's and Modern Warfare 2's v0.1.5, and Call of Duty 4's first
release, v0.1.0, from the same commit.

### Highlights
Modern Warfare 3 and Modern Warfare 2 (plain text: the app's update dialog
shows a release's notes as they are written).

- Online: each copy now uses a network port of its own, so two devices on
  the same Wi-Fi can both reach the server. Before, the second one timed
  out ("did not answer within 5 seconds") and stayed offline.
- A memory leak in the shared online code is fixed. It ended long Call of
  Duty 4 matches; ten-minute Modern Warfare 3 and 2 matches showed no sign
  of it.
- Setup moves the game files aside in a second when it replaces an app
  signed with another key, instead of copying 6 to 8 GB out and back.

The game package built for v0.1.0 to v0.1.4 still fits: nothing to build
again.

### Added
- **Call of Duty 4: Modern Warfare, a third game**, with a public repository
  of its own, `yassinsolim/cod4-recomp`, whose releases carry its app
  (`com.cod4.recomp`, v0.1.0 first; `README-COD4.md` is that repository's
  README). `setup\setup.cmd -Game cod4` builds the game code from your own
  disc for Android and Windows and downloads the published app, which
  updates itself from there. The campaign and a System Link match play;
  `docs/COD4-BRINGUP.md` says what has been checked and what has not.

### Fixed
- **Call of Duty 4, before its first release**: the larger campaign levels load
  instead of ending in "Out of memory"; the multiplayer starts on Android,
  and a System Link search finds a host on another device; a core no longer
  spins waiting for the sun's occlusion count at 120 frames a second.
  Measured on the AYN Thor and the Retroid Pocket Flip 2
  (`docs/COD4-BRINGUP.md`). Modern Warfare 2 and 3 are unchanged.
- Call of Duty 4's multiplayer: the app's pause menu no longer offers Modern
  Warfare 2's Max rank, Unlock all and Prestige entries, which write that
  game's stats file.
- Setup, replacing an app signed with another key (all three games): the
  game files and saves are moved aside beside the app's folder, a rename
  that takes a second, where the place it used before made Android copy all
  6 to 8 GB out and back (minutes, and long enough to time out).
- **Online from a second device on the same Wi-Fi** (all three games): every
  copy came to the relay from the same port, and behind some routers the
  second one in a household got no answer ("did not answer within 5
  seconds") and stayed offline until restarted. Each copy now uses a port
  the system picks.
- **Call of Duty 4's Xbox LIVE Find Match** through a relay: the playlists
  offered were Modern Warfare 2's, with maps this game does not have, and a
  Team Deathmatch lobby stayed at "Match beginning in: 0" because the host's
  read of the players' skill was never answered in a form it takes. It has
  its own playlists (Team Deathmatch and Free-for-all on the disc's sixteen
  maps) and the read is answered with no rows. Two players who search in the
  same second no longer end in a lobby each.
- **Call of Duty 4's multiplayer ended every match after about five
  minutes** on a 120 Hz device (ten on a 60 Hz one) with "Xbox 360 Error 6
  while trying to enumerate lsp title servers", and the host's screen stayed
  black: memory the game takes each frame while it looks for a Live server
  was never given back.
- Call of Duty 4's app: the campaign's entry no longer says Special Ops,
  which this game does not have.
- The launcher (all three games) no longer starts the game a second time
  when you quit the run that INSTALL & PLAY started.
- The repository's "MW3 validation" checks build and run again: the fakes
  of two unit tests had fallen behind the runtime (`kernel_xam_test` had no
  stand-in for the voice chat, the network trace and the lobby server;
  `gpu_command_processor_test`'s `Swap` had the old signature).

## [v0.1.4] - 2026-11-23

### Highlights
- **Bots in Modern Warfare 2's multiplayer.** Set them under Multiplayer →
  BOTS on the start screen (how many, up to 17, and how good), then host a
  System Link or private match. They join, pick a team and a class, fight,
  score, and leave to make room for real players. The pause menu changes
  their number in a match; Settings → Multiplayer has the same settings.
- **Their limits**: the maps have no path data for them, so they feel their
  way round walls and are easier to find on a small map. No grenades,
  killstreaks or objectives yet. The disc version only, which is what this
  app is. They join only the matches your device hosts.
- **Modern Warfare 3's bots run on the code Modern Warfare 2's now share.**
  How they fight and score, and the rule that a bot leaves to make room for
  a player, are unchanged.

The game package built for v0.1.0 to v0.1.3 still fits: nothing to build
again.

### Added
- **Bots in Modern Warfare 2's multiplayer.** The launcher's **Bots** entry
  and Settings → Multiplayer are in Modern Warfare 2's app as well: choose
  how many (up to 17) and how good, start a System Link or private match, and
  the bots join it, pick a team and a class, fight, score and leave to make
  room when a player joins. In a match the app's pause menu changes the
  number. On the PC: `MW2_BOTS`, `MW2_BOT_SKILL`, `MW2_BOT_TEAMS`. The disc
  version only, which is what the app is; `runtime/bots.cpp` plays both
  games' bots now, and Modern Warfare 2's addresses are in `runtime/title.h`
  with how each was found ([docs/multiplayer.md](docs/multiplayer.md#modern-warfare-2s-bots)).
  Modern Warfare 2's bots are handed a full load of ammunition when they have
  fired their last round; they would otherwise face their target for the rest
  of their lives.

### Changed
- Modern Warfare 2's multiplayer, disc version, diagnostics builds:
  `MW2_ENGINE_LOG` shows the title's other three prints as well (the server's
  reasons for refusing a connect among them).

## [v0.1.3] - 2026-11-23

### Highlights
- **Custom camos are remembered again.** Since v0.1.2 the choice was dropped
  at every start, so a pack's camouflage showed as plain Gold. Pick yours
  once more after updating.
- **Pack camos shine like Gold** instead of looking near black. The packs
  are unchanged: nothing to fetch or import again.
- **Modern Warfare 3's campaign**: Resume Game no longer stays black after
  the briefing when a mod is switched on.
- **Windows build**: it links again.

The game package built for v0.1.0 to v0.1.2 still fits: nothing to build
again.

### Fixed
- **Resume Game in the campaign no longer stops on a black screen when a mod
  is on.** With any mod turned on in the mods folder (a multiplayer-only one
  included), resuming a campaign save made without it played the briefing
  and then stayed black at 0 FPS; v0.1.2 does the same. The room set aside
  for the mods' scripts lay under the game's own, and a save holds every
  script's position by its distance from the start. The room now lies above
  them, so the game's scripts are where the save expects them. New Game and
  Mission Select were not affected.
- **A pack camouflage stays on the weapon after a restart.** In v0.1.2 a
  camouflage chosen from a pack (the project's own thirteen included) was
  drawn in the run it was chosen in and was plain Gold from the next start
  on, in Create a Class too. At sign-in the game's menus tidy every custom
  class, and on a phone that happened before the stats had loaded; the app
  took it for the player choosing no camouflage and forgot the choice.
  Choose the camouflage once more after updating. The log now says what the
  weapons wear whenever it changes, and why a choice was dropped.
- **Camo packs no longer look near black on a weapon.** A pack camouflage is
  drawn through Gold's image, whose alpha is Gold's gloss; a PC mod's maps
  are opaque, which the game read as a mirror's gloss, so Interstellar and
  Bubble Gum showed only pinpoints of light. A picture without alpha of its
  own is now drawn with Gold's gloss. The packs themselves are unchanged:
  nothing to fetch or import again.
- The Windows build links again (`platform::CoreCount` had no Windows
  definition).

## [v0.1.2] - 2026-11-22

### Highlights
- **Far fewer stutters, and none of the long ones.** Shaders compile in the
  background and what is compiled is saved as you play; a network thread
  that took a whole processor core in every multiplayer match now sleeps.
  A first Seatown match on an AYN Thor: 550 hitches to 91, the longest
  463 ms to 68.
- **Unlock all covers everything in Modern Warfare 3's multiplayer**: every
  strike package, fifteen custom classes, all camouflages (Settings,
  Multiplayer; on unless you switch it off). Your stats are not changed.
- **Bots are easy to find**: a Bots entry under Multiplayer on the start
  screen, a Multiplayer tab in Settings, and the pause menu changes their
  number in a match.
- **Camo packs**: every camouflage of every pack is an entry of its own
  below Gold, and nothing of the game's is replaced. Camo packs (Settings,
  Multiplayer) adds any PC pack from a .zip, .iwd or .iwi files, and
  fetches Solid Colours and Bubble Gum by Sloth, credited with his threads.
- **Animated camos**: a still camouflage can scroll, shimmer or cycle its
  hue.
- **Less texture memory** on Turnip: compressed textures stay compressed.

The game package built for v0.1.0 or v0.1.1 still fits: nothing to build
again.

### Changed
- **Unlock all covers the strike packages**: every Assault, Support and
  Specialist streak can be ticked and is given in a match (they needed the
  rank's unlock tokens: only each package's first three could be chosen).
  Also **fifteen custom classes** instead of five, and the **Marine and
  Winter camouflages**. As before nothing is written to the stats. Prestige
  Mode and its shop are left as earned. `MW2_TRACE_UNLOCKS=1` (diagnostics
  builds) logs what the title asks and would have answered.
- **The camo packs' credits list the pack the app carries** before the
  multiplayer has first copied it into the game folder.
- A camouflage's preview follows the focused row in the same frame.
- `pack.json` has no `permission` line; `camos/options.txt` holds the
  player's choices about packs.
- **The hosted camo packs are in the public repository's `camo-packs`
  release** (`yassinsolim/mw3-recomp`; `release` in `setup/camo-packs.json`):
  setup and the app's *Get the project's packs* fetch them from there, and
  the project's own pack links to that repository
  ([docs/android-releases.md](docs/android-releases.md#the-camo-packs-release)).

### Fixed
- **Far fewer stutters, and none of the long ones** (both games, most of all
  multiplayer on a Snapdragon with Turnip). On an AYN Thor a first Seatown
  match went from 550 late frames in four and a half minutes, the longest
  463 ms, to 91 and 68 ms; a second session from 436 and 1.3 s to 54 and
  81 ms. Modern Warfare 2's Afghan: 286 and 814 ms to 86 and 83 ms.
  - A network thread that never slept took a whole processor core in every
    multiplayer session, usually the one the renderer was held to.
  - Shaders new to a level are compiled on up to three threads and never on
    the renderer's; its surfaces appear over the first two or three seconds
    of a first visit instead of every frame waiting for one.
  - What is compiled is saved every ten seconds, when the game is paused and
    when its window goes, so a game swiped away or killed keeps it; a level
    played before is ready within a second of the start.
  - Turnip samples compressed textures itself (a quarter of the texture
    memory), and Modern Warfare 3's lighting volume is read in parts.
  - New switches: `MW2_COMPILE_THREADS`, `MW2_ONE_PIPELINE_CREATION`,
    `MW2_ADRENO_BC=sw` (docs/switches.md).

### Added
- **Bots are on the start screen** (Modern Warfare 3 multiplayer): a
  **Bots** entry under **Multiplayer** sets how many, how good and whether
  they all play against the players, and shows what is set. They were at the
  end of Settings' Graphics page, below Sound. Settings has a
  **Multiplayer** tab now (Bots, Unlocks, Matches, Match options), and the
  pause menu changes the number of bots in a match. Bots join the matches
  this device hosts: System Link, Create Game, or a private match.
- **Any PC camo pack, without clashes** (Modern Warfare 3 multiplayer). Every
  camouflage of every pack is an entry of its own below Gold, named with its
  pack (`SOLID - RED`, `INTERSTELLAR - SLOTH`): a PC mod's replacement of
  Gold or of one of the game's camouflages becomes a new camouflage, so the
  game's thirteen stay as they are and two packs made for the same slot are
  two entries. The list is no longer held to forty.
- **Adding a pack**: the app's Camo packs screen (Settings, Multiplayer) adds
  one from a `.zip`, `.iwd` or loose `.iwi` files and converts it on the
  phone, lists each pack with previews, author and link, and switches packs
  off; `setup -CamoPacks <folder>` (or `setup\camo-packs\`) converts zip, rar,
  7z, iwd and loose files in any layout. `tools/import_pack.py` works out
  what each image was made for by its name.
- **Packs by Sloth and jdhruv**, credited with their threads: Solid Colours
  and Bubble Gum are fetched by setup and the app; Interstellar, Exclusion
  Zone, Classic Camos and Diamond, which follow other Call of Duty games'
  camouflages or the game's own textures, are imported from the player's own
  download. Diamond's whole-weapon skins become one camouflage with a tile a
  weapon.
- **Animated camos**: a still camouflage can scroll, shimmer or cycle its
  hue (`"animation"` in pack.json), drawn by the port's renderer. *Animate
  Gold-type camos (Sloth's style)* gives every camouflage made for Gold the
  scrolling look of Sloth's Animated Camos Mod; *Animate* on a pack does it
  for that pack.

## [v0.1.1] - 2026-11-21

### Highlights
The game package built for v0.1.0 still fits: nothing to build again.

- **Faster on every Adreno.** Modern Warfare 3's Survival: 116 to 118
  frames a second on an AYN Thor at 120 Hz (it was 87), 59 to 60 on a
  Retroid Pocket Flip 2 (46). Modern Warfare 2: S.S.D.D.'s opening 100 to
  106 on the Thor, Team Player's flight 53 to 59.5 on the Flip 2.
- **The FPS cap follows the screen**: 120 on a 120 Hz panel, 60 on a 60 Hz
  one. A cap saved before is dropped once.
- **Adreno 6xx**: both games draw an 832x480 scene.
- **Modern Warfare 3 leaves the sun's shadows out above 60**; Sun shadows
  in Settings (Graphics) gives them back.
- **Shaders**: no freeze of seconds at a level's first visit while they
  compile, and the compiled ones are kept past 64 MB.
- **The start screen shows your game's own artwork**, from your disc.
- **Online**: the host of a match no longer freezes at its first frame,
  and Modern Warfare 2's Find Match lists its playlists.
- **Install & Play** starts the game on a slower phone too.
- **A clear message when the game code is missing**, and better crash logs
  to share.
- **No missing scenery** in Team Player.
- **Detail at distance** (Modern Warfare 2; Settings, Graphics): coarser
  models sooner.
- **Map vote** (Modern Warfare 3 multiplayer): the game modes the disc left
  unfinished are not offered.
- **Mali**: a G57 or newer meets every requirement on paper; a G52, G72 or
  G76 is refused.

### Changed
- **The FPS cap starts at the screen's own rate**: 120 on a 120 Hz panel (AYN Thor), 60 on a 60 Hz one. Modern Warfare 3 on an Adreno 6xx no longer starts at 30, since it now draws 56 to 58 under a cap of 60 there. A cap saved before this is dropped once, since the old defaults were saved as readily as a choice (a Thor was left at 60 and a Flip 2 at 30); one chosen from now on is kept.
- **Modern Warfare 3 holds those caps better.** On an Adreno 6xx it draws the 832x480 scene Modern Warfare 2 already had there (`MW2_SCENE` now works in Modern Warfare 3): Survival on the Flip 2 from 55.8 frames a second with 37% of them on time to 59.9 with 77%. Above 60 it leaves the sun's shadows out: Survival on the Thor from 106.0 with 37% on time to 118.2 with 87%, Black Tuesday from 114.9 to 118.4. **Sun shadows** in Settings (Graphics) gives them back, or takes them away anywhere ([docs/android.md](docs/android.md#adreno-7xx-snapdragon-8-gen-2-class)).
- **A first visit to a level no longer freezes for seconds** while its shaders compile: the renderer builds one overdue pipeline a frame, not hundreds in one. The longest frame of a first Survival match went from 7.0 s to 91 ms on the Flip 2 and from 5.8 s to 81 ms on the Thor; the first seven seconds run slowly instead.
- **A screen the system holds under the FPS limit** (the AYN Thor's display setting at 60 Hz) is drawn one frame a blank, not 125 a second to show 60, and the app says at the start of a run that the screen is set to 60 Hz ([docs/switches.md](docs/switches.md), `MW2_FPS_LIMIT`).
- **The launcher's corner credit is the same in both apps**: "Built on
  mw2-recompiled · mw2-android · XenonRecomp". Modern Warfare 3's named the
  runtime and Modern Warfare 2's one author; who wrote each is in the
  README's credits.
- **Modern Warfare 2's app has its own launcher emblem**
  (`android/app/src/mw2/res/drawable`): two stencil bars in a blue ring.
  Both apps drew the amber three-bar one and could only be told apart by
  name on one home screen.
- `MW2_TRACE_MATCH` also says where each player landed.
- **No draw is skipped for want of frame arena**
  ([docs/rendering.md](docs/rendering.md#a-frame-larger-than-its-slots-share)).
  Modern Warfare 2's Team Player skipped 34,000 draws in a flight of two
  minutes on the AYN Thor and 8,500 on the Retroid Pocket Flip 2
  (`draws skipped: frame arena full`): frames with the far buildings and the
  sky missing at the level's start, after a checkpoint loaded again and
  whenever a frame slot's kept vertex data had filled its share. A draw its
  slot's share has no room for is now made in the next slot, after a
  submission, and a slot keeps vertex data by blocks of guest memory that
  lie apart, uses their places again, copies written pages again one by one
  and lets the ranges it has not drawn from lately go first. The same flight,
  S.S.D.D. and Modern Warfare 3's Survival skip none on either device.
- **The arena as texels on every Adreno under Turnip**, not the 6xx alone
  (`MW2_VERTEX_TEXELS`, `MW2_PIXEL_TEXELS`,
  [docs/android.md](docs/android.md#adreno-7xx-snapdragon-8-gen-2-class)).
  On the AYN Thor's Adreno 740, both against neither: 7.4 ms of GPU a frame
  for 9.1 in S.S.D.D.'s opening, 10.4 for 13.2 at Team Player's heavy views,
  and Modern Warfare 3's Survival at 108 frames a second for 87 (8.5 ms for
  10.9), with the same picture.
- **A pixel shader on an Adreno 6xx reads its draw's record as texels**
  ([docs/shaders.md](docs/shaders.md#the-module-interface),
  `MW2_PIXEL_TEXELS=0|1`; the default there under Turnip, off elsewhere,
  where a module is the same bytes as before). A lit surface's pixel shader
  fetched 26 to 44 single dwords of the storage buffer a pixel, asked each
  texture its size and computed a reciprocal pair and an `exp2` for it. The
  record now holds one texel a texture with the slot's word, the fetch
  constant's LOD bias and exponent adjust and one over the image's size,
  and the alpha test's four words are one fetch. On the Retroid Pocket
  Flip 2, Modern Warfare 2's Team Player holds 59 to 60 frames a second at
  its heavy views for 48 to 51 (15.3 ms of GPU a frame for 18.5), and its
  flight is 59.5 with 87.9% of the frames on time for 56.6 and 69.3%, its
  first half 59 for 51; S.S.D.D., Terminal and Rust hold 59.9 to 60.0. The
  picture is the same. Modern Warfare 3 there: Survival with the limit at
  60 is 58.2 frames a second for 46.1 and Black Tuesday's opening 55.8 for
  46.3; at its limit of 30 it holds 30 with 6 to 7 ms less of GPU a frame.
- **Windows: the mouse is a mouse** ([docs/runtime.md](docs/runtime.md#the-mouse)).
  It turned the view by driving the right stick, through the title's
  response curve and turn acceleration: a thousand counts in a second
  turned 28 degrees where the PC game turns 110, and a slow hand hardly
  turned it at all. The counts are now an angle added to the view where the
  title works it out, in the disc's four executables: 0.022 degrees a count
  times `MW2_MOUSE_SENSITIVITY`, the title's Sensitivity option and a
  sight's zoom.
- **Windows: the mouse works the menus.** With a menu up the left button is
  A, the right one B and the wheel the d-pad. In Modern Warfare 3 the cursor
  is let go and points: what is under it has the focus and a click acts on
  it. In the world the wheel switches weapon.
- **Escape** opens the pause menu (Start) in the world and goes back (B) in
  a menu; it was the Back button. Tab and Backspace still are.
- **A vertex shader on an Adreno 6xx reads the frame arena as texels**
  ([docs/shaders.md](docs/shaders.md#the-module-interface),
  `MW2_VERTEX_TEXELS=0|1`; the default there under Turnip, off elsewhere,
  where a vertex shader's module is the same bytes as before). Turnip reads
  a storage buffer one dword a fetch on that chip, three instructions each,
  and a vertex's loads waited on each other four deep. A float constant is
  now one texel of a buffer view, the draw table's entry two texels that
  hold what the vertex loads next, and vertex data four dwords a texel.
  Modern Warfare 2's commonest vertex shader went from 207 instructions
  and 45 fetches a vertex to 99 and 11, the lit world's from 348 and 61 to
  204 and 16; what a vertex computes is the same and so is the picture. On
  the Retroid Pocket Flip 2, Team Player's flight is 56.4 frames a second
  with 69% of the frames on time for 53.0 and 55%, its heavy views 50 to
  53.5 for 44 to 46; S.S.D.D., Terminal and Rust hold 59.5 to 59.9. Modern
  Warfare 3 there holds its 30 either way with 1.5 to 2 ms less of GPU a
  frame (Survival 25.3 to 23.8 ms, Black Tuesday 25.1 to 23.1, Seatown with
  bots 20.0 to 18.3), and with the limit at 60 Survival is 46.4 frames a
  second for 42.3 and Black Tuesday 45.3 for 42.1; the picture is the same.
- **The Retroid Pocket Flip 2 at 60, measured again**
  ([docs/android.md](docs/android.md#modern-warfare-2-there-60-frames-a-second)):
  Turnip's autotune, `sysmem`, `nobin`, `nolrz` and `noearlypreamble`,
  TurnipZ 0.4.0, image compression by image class and a depth clear that
  keeps LRZ change nothing in Modern Warfare 2's Team Player; Turnip 24.1.0
  R18 and `gmem` are slower. No default changed. With vertex data in host
  byte order S.S.D.D. holds 59.8 frames a second for five minutes, Terminal
  59.5, Rust 59.4, and Team Player's flight 52.6: its opening is still 37
  to 50.
- **Mali, worked out without one** ([docs/android.md](docs/android.md#mali),
  [docs/TESTING-MALI.md](docs/TESTING-MALI.md)): `tools/check_device_report.py`
  walks the runtime's start-up checks against a phone's report from
  vulkan.gpuinfo.org, and `tools/malioc_shaders.py` compiles every translated
  shader with Arm's Mali Offline Compiler. A Mali-G57 or newer meets every
  requirement and its compiler takes every shader of both games; a Mali-G52,
  G72 or G76 has no descriptor indexing and is refused. A Mali is given a
  draw record a pixel (`MW2_PIXEL_RECORDS`) and its pixel constants as a
  uniform block; Turnip is not loaded where the chip is not an Adreno; the
  mirror-once address mode has its extension enabled; and the log's
  `device:` line names the chip, the driver and every fallback chosen.
- **Renderer, measured on the AYN Thor at 120 Hz with Modern Warfare 2**
  ([docs/ANDROID-PROGRESS.md](docs/ANDROID-PROGRESS.md), 2026-11-20): a
  segment of the frame that nothing was uploaded for goes on in the command
  buffer and rendering instance of the one before (`MW2_JOIN_SEGMENTS=0` is
  the old behaviour); a texture the title rewrites a few pages of is read
  again and staged in those pages only (`MW2_TEXTURE_PARTS=0`); an Adreno has
  a pixel shader's float constants as a uniform block (`MW2_PIXEL_UNIFORMS`);
  no fence status is asked for without a shadow of guest memory. S.S.D.D.
  flown for five minutes: 115.9 frames a second, 85% of them a display
  period apart; its opening 84.7 to 93.1.
- **Renderer: what a vertex loads** ([docs/shaders.md](docs/shaders.md)):
  a vertex program loads a word of its constant table once for each block
  of constants it reads instead of at every constant, its window transform
  as one vec4, one dword for a vertex fetch, and exchanges a dword's bytes
  in two steps. 66 loads from the arena a vertex became 27. On the AYN Thor
  Modern Warfare 2's S.S.D.D. opening went from 93.7 to 104.1 frames a
  second and Team Player's heaviest views from 65 to 71; the Retroid Pocket
  Flip 2 is not measured yet. `MW2_SHADER_STATS` counts the loads by kind.
- **Renderer: vertex data in host byte order**
  ([docs/rendering.md](docs/rendering.md#vertex-data-in-host-byte-order)):
  where there is no shadow of guest memory (Turnip) the renderer exchanges
  the bytes of vertex data as it copies it into the arena, as each fetch
  constant's endianness says, and the vertex shaders load a dword as it
  lies instead of exchanging it for every vertex. On the AYN Thor Modern
  Warfare 2's S.S.D.D. opening went from 103.3 to 105.9 frames a second,
  Team Player's heaviest views from 11.5 to 11.0 ms for the scene, and
  Modern Warfare 3's Survival from 87.3 to 91.7 frames a second; on the
  Retroid Pocket Flip 2 Team Player's views went from 17.0 to 15.8 ms and
  42.7 to 45.6 frames a second. The picture is the same. `MW2_HOST_ORDER=0`
  is the old behaviour; `translate-shader --host-order` builds that form of
  a shader. Vertex attributes in place of arena loads were measured with
  it and lost on both the Adreno 740 and the 650, so they are not here.
- **Adreno 6xx: Modern Warfare 2 starts at 60 frames a second**, without the
  sun's shadows and with an 832x480 scene under the 1280x720 frame
  (`Prefs.fpsLimit`, `MW2_SHADOWS`, `MW2_SCENE`). On a Retroid Pocket Flip 2
  S.S.D.D. ran at 43 frames a second with the cap at 60; it runs at 58 over
  five minutes, Terminal and Rust at 59, three frames in four and nine in ten
  on time ([docs/android.md](docs/android.md#adreno-6xx-snapdragon-865-class)).
  Modern Warfare 3 still starts at 30 there.
- **A pixel shader's float constants are a uniform block on a device without
  subgroup ballot in the fragment stage** (`shader::PixelConstants`,
  `bindings::kPixelConstants`, [docs/shaders.md](docs/shaders.md#the-module-interface)):
  there every pixel fetched each constant it read from the arena, some forty
  fetches a pixel, a quarter of the GPU's frame on an Adreno 650 (21.2 ms to
  15.4 on S.S.D.D.). Set 2 is bound again at a draw whose pixel constants
  differ from the last's, which ends the run of draws before it: one join in
  thirty. `MW2_PIXEL_UNIFORMS=0|1` says otherwise on any device.
- **A frame slot keeps the vertex data it copies where there is no shadow of
  guest memory** (Turnip: `renderer_arena.cpp`, `gpu::watch::TrackSteady`,
  [docs/rendering.md](docs/rendering.md#vertex-data-kept-without-the-shadow)):
  every draw copied its vertex windows there, half of the ring consumer's
  time on a Snapdragon 865, and Team Player's did not fit a slot's share of
  the arena, so its sky and far buildings were not drawn. `MW2_KEEP_VERTICES=0`
  copies as before. A phone of 7 GB and more has the 384 MB arena.
- **The driver's pipeline cache is written to its file when it has grown**,
  not after every five pipelines the warm-up found in it (`SaveCache`).
- **`MW2_SCENE=<w>x<h>`** (`runtime/render_size.cpp`): the 3D scene the title
  draws before it copies it into its frame, by itself, the frame and with it
  the HUD and the menus staying as they are. `MW2_RENDER` and it now reach
  the disc version of Modern Warfare 2 as well (`runtime/title.h`).

### Fixed
- **Setup on a phone without the game files yet** (`setup/setup.ps1`) says
  so about the camouflage packs of other authors, as it meant to. It said
  "could not copy the camouflage packs: adb.exe failed with exit code 1":
  the test for the game folder was a failed command when the folder was
  not there, which is every first install.
- **The Play card after Install & Play** says the game is installed and
  which version, not "Game Disc Detected ... Ready for one-click setup",
  which it kept from before the install.
- **A match no longer freezes on a write the runtime answers for ever**
  (`runtime/gpu/memory_watch.cpp`, [docs/rendering.md](docs/rendering.md#the-shadow-of-guest-memory)).
  A page of guest memory could be left marked as written while still
  read-only, when it was copied again between the two steps of making a range
  writable for the image pool's moves, or between the two steps of a write
  fault's answer. The next write to it faulted, was answered as handled, and
  faulted again: the title's render backend spun there and the picture
  stopped. Seen on the AYN Thor hosting a Find Game match of Modern Warfare 2
  with the Retroid Pocket Flip 2, at the match's first frame; the other
  player was dropped with "Server connection timed out".
- **Modern Warfare 2's Find Game has its playlists on a relay that has none
  for it** (`runtime/online/server.cpp`, [docs/relay.md](docs/relay.md)): the
  project's relay is started without a playlists file, and FIND MATCH listed
  nothing ("Maximum party size of 0"). A relay that says it has none now
  leaves each copy the list its release was built with, as Modern Warfare 3
  and Steam already had.
- **Install & Play starts the game on a slower phone**
  (`Prefs.gameDirectory`): the installer started the game before the game
  folder's setting was on disk, and the game's process, which reads the file,
  said "The game files are not installed" (Retroid Pocket Flip 2, first
  install).
- **Windows, the mouse**: `Alt+Tab` no longer opens the scoreboard (its Tab
  was taken as the pad's Back before the window lost the keyboard), and
  with the scoreboard up the mouse still turns the view and fires, the
  cursor staying hidden, as the controller does there. Found with real
  Windows input (`SendInput`) in Modern Warfare 3's multiplayer, window and
  fullscreen ([runtime.md](docs/runtime.md#the-mouse)).
- An imported Turnip asked to disassemble its shaders
  (`IR3_SHADER_DEBUG=disasm`) aborted the game at the first pipeline: the
  driver's file redirect had no `dev` folder for the `/dev/null` its
  disassembler opens.
- **A game package holds its own game's libraries only**
  ([docs/game-library.md](docs/game-library.md)): setup kept both games'
  libraries in one folder and packaged the folder, so a PC that had built
  both made a 98 MB Modern Warfare 2 package of what is 40 MB.
- **The driver's pipeline cache is bounded and no longer begun again**
  ([docs/rendering.md](docs/rendering.md#the-drivers-cache-in-several-files),
  `MW2_PIPELINE_CACHE_MB`): a cache file over 64 MB was not loaded, by this
  runtime's own limit and not the driver's, and every pipeline was compiled
  again (633 of them in 54 s of a 100 s run on an AYN Thor, one frame of
  2.9 s). On Android the cache is now several files under a budget of
  64 MB; the ones nothing is found in go, and the log and a report count
  the pipelines found and compiled. A second run of Modern Warfare 2's
  S.S.D.D. compiles 0 to 2 of 1,574, Modern Warfare 3's Survival 1 to 3 of
  513.
- **MW3 multiplayer: a vote does not offer the game modes the disc left
  unfinished** (`runtime/map_vote.cpp`): One Flag CTF, Arena, Global
  Thermonuclear War and the two that do not start were drawn with the
  others unless `MW2_MAP_VOTE_MODES` said which.
- **A run that is not started says why in the log**, not on the screen
  alone. The first report from a Mali phone was a `game.log` that ended
  after the surface's two lines: no crash, but the app showing "The game
  code is not on this device" after Install & Play, with nothing logged.
  The log now has `app: the run was not started: ...`, and Install & Play
  and the launcher's Multiplayer entry say the game code is missing instead
  of starting a run that cannot go.
- **A fault's report reaches the log before the process ends**
  (`runtime/crash.cpp`, `runtime/android/log.cpp`). On Android it is printed
  through a pipe another thread reads into `game.log`, and the process
  exited without waiting for that thread; `abort()` (a driver giving up, an
  uncaught exception) was not reported at all.

### Added
- **The start screen shows the game's own artwork** once the game files are
  installed (`DiscArt.kt`, `BackdropView.kt`): the backdrop the console's
  dashboard shows for the disc, `nxebg.jpg` in the disc's `nxeart` package,
  which the installer already copies. It is read from the player's own
  files on the phone and kept in the app's private folder: never in the
  APK, never in the repository. Without the game files the launcher keeps
  its haze, and the home-screen icon stays the project's emblem.
- `MW2_COUNT_PRIMITIVES=1`: at exit, the draws and triangles of a frame in
  play, by culling, pass and size of draw.
- `MW2_INPUT_SCRIPT` takes the mouse on Windows (`at<x>/<y>`, `click`,
  `turn<dx>/<dy>`), `MW2_TRACE_POINTER` lists what a menu offers the
  pointer, and `MW2_TRACE_MATCH` says how far down a player looks
  ([docs/switches.md](docs/switches.md)).
- `MW2_SHADER_IR=<dir>` (with `MW2_SHADER_STATS=1`): what the driver
  compiled each vertex shader to, a file each -- on Turnip the NIR and the
  ir3 assembly, binning variant apart.
- **Detail at distance** (Modern Warfare 2; Settings, Graphics;
  `MW2_LOD_BIAS`, [docs/rendering.md](docs/rendering.md#detail-at-distance)):
  models change to their coarser meshes at half or a quarter of the
  console's distances, and nothing disappears sooner. The console's by
  default. On the AYN Thor the opening of S.S.D.D. runs at 111 frames a
  second at half against 102.
- **A report from a phone says what it is and how the run ended**
  ([docs/android.md](docs/android.md#what-the-log-share-holds),
  [docs/TESTING-MALI.md](docs/TESTING-MALI.md)). The Log screen's Share
  opens with the app's version, the phone, its chip, memory, GPU and driver,
  and whether the game code package is there; then Android's own record of
  how the last runs ended (killed for memory, crashed, exited), with a
  native crash's signal and backtrace out of the system's tombstone.
  `game.log` has a `step:` line before each part of start-up, a fault's
  report names the last one and covers `abort()`, and
  `MW2_DEBUG_CRASH_AT=<step>` faults at one on purpose.

## [v0.1.0] - 2026-11-20

### Added
- **Modern Warfare 2 on Android: one app that updates itself**
  ([docs/MW2-ANDROID.md](docs/MW2-ANDROID.md), [README-MW2.md](README-MW2.md)):
  the campaign, Special Ops and the multiplayer are one app (`com.mw2.recomp`,
  the `mw2` flavour; `libmw2.so`, `libmw2mp.so`) built without the game's
  code, as Modern Warfare 3's is, and published from `yassinsolim/mw2-recomp`
  (`.github/workflows/android-release-mw2.yml`); the recompiled code is the
  package `com.mw2.recomp.game`, which `setup\setup.cmd -Game mw2` builds from
  the disc. The game's menu entries between the campaign and the multiplayer
  restart the run with the other library, and one copy of the game files
  serves both. Setup installs the published app, or builds it when there is
  no release yet, and copies the saves of the two apps of old. Title update 6
  still builds as an app for each title (the `campaign` and `multiplayer`
  flavours). `-DMW3_BOTH_TITLES` is now `-DMW2_BOTH_TITLES`, and
  `-Pmw3Suffix` `-PappSuffix`.
- **MW3 on Android: an app that updates itself** ([docs/game-library.md](docs/game-library.md),
  [docs/android-releases.md](docs/android-releases.md)): the app is built
  without the game's code and published from the public repository's
  releases; the recompiled code is a package of its own
  (`com.mw3.recomp.game`) that `setup\setup.cmd` builds from the disc and
  the app loads. The two are checked against each other when the game starts,
  and a game package built for another version is refused with what to do
  (`setup\setup.cmd -GameOnly`). The app looks for a newer release when it
  starts and from Check for updates, shows the notes and installs it over
  itself (the download used to finish and never install: its broadcast was
  not received).
- **Modern Warfare 2 on Android, again** ([docs/MW2-ANDROID.md](docs/MW2-ANDROID.md)):
  the campaign and multiplayer apps build from the disc's executables with
  `setup\setup.cmd -Game mw2`, carry the Turnip driver Modern Warfare 3's app
  carries and use it by default on Qualcomm phones (the phone's own driver
  left surfaces out for minutes while it compiled their pipelines), and each
  launcher finds the other app (it looked for a package name the apps never
  had). The disc's campaign has its player position (`runtime/title.h`), so
  recorded flights and `MW2_TRACE_VIEWPOS` work in it.
- **MW3 multiplayer: match options** (`runtime/match_options.cpp`; the app's
  "Match options"): Game Setup shows the Alternate Modes the disc hides (Gun
  Game, Infected, One in the Chamber, Juggernaut, Team Juggernaut, Drop Zone)
  and its two class menus; the host sets gravity, movement speed, jump
  height, unlimited sprint and fall damage for its matches (`MW2_MATCH_*`,
  `match ...` on the host's console), sent to every player; `MW2_CUT_MODE`
  starts three modes the disc left unfinished
  ([docs/MW3-MP-BRINGUP.md](docs/MW3-MP-BRINGUP.md#match-options)).
- **MW3 multiplayer: field of view** (`MW2_FOV`, the app's slider): lasts
  through every spawn, with the turn rate from the hip unchanged and a
  sight's following the wider view ([docs/gameplay.md](docs/gameplay.md#field-of-view)).
- **MW3 multiplayer: map vote and rotation** (`MW2_MAP_VOTE=1`,
  `MW2_MAP_ROTATION`, `MW2_MAP_VOTE_MODES`, `MW2_MAP_VOTE_SECONDS`,
  `runtime/map_vote.cpp`; the app's "Vote for the next map after my matches",
  off by default): a System Link or private match this copy hosts ends in a
  vote between three maps and game types, answered with the tactical, use or
  lethal button, and the winner loads with every player and bot still in the
  match instead of everyone being dropped to a menu
  ([docs/multiplayer.md](docs/multiplayer.md#the-next-map)).
- **MW3 multiplayer: camouflage packs** (`runtime/mw3_custom_camos.cpp`,
  `runtime/camo_packs.cpp`, `tools/import_pack.py`; `MW2_CUSTOM_CAMOS=0` turns
  it off): the camouflages of the packs in the game folder's `camos` are
  listed below Gold in Create a Class, each with its own name, preview and
  description, chosen per class and weapon and drawn on the weapon in a
  match. None of the game's camouflages is replaced and only Gold is written
  to the stats. The project's own pack of thirteen is drawn by
  `tools/camo_gen.py --pack` at set-up (new styles: nebula, neon grid, toxic,
  magma, glitch, aurora, frost) and never committed; a pack of somebody
  else's is fetched from its author's link by `setup/setup.ps1`
  (`setup/camo-packs.json`) or made from a PC mod by `tools/import_pack.py`,
  and credited in the game and under the app's Settings, Camo packs. One
  custom camouflage is drawn at a time on a device, and other players see
  Gold ([docs/textures.md](docs/textures.md#camouflage-packs)).
- **MW3: a mods folder** (`<game>/mods/<name>/`, turned on in `mods/mods.txt`;
  `runtime/mods.cpp`): a mod's `.gsc` scripts replace the fastfiles' own by
  path or add new ones, `scripts/` scripts run `main()` and `init()` as a
  level starts, and other files replace raw files. Scripts are compiled in
  the game by gsc-tool (xensik, GPL-3.0), since this build loads compiled
  scripts only. `MW2_SCRIPT_DUMP` writes the game's scripts as source,
  `MW2_TRACE_MODS` logs what the game asks for. The launcher's MODS installs
  and toggles a script mod. Host only; experimental
  ([docs/mods.md](docs/mods.md)).
- **MW3 on Android: one app for the whole game** (`com.mw3.recomp`): the
  campaign with Special Ops and the multiplayer are two native libraries in
  one APK (`libmw3.so`, `libmw3mp.so`; `-DMW3_BOTH_TITLES=ON`,
  `cmake/android.cmake`), with one copy of the game files. The launcher
  offers both; the game's own menu entries between the two restart the run
  with the other library (`HandOffActivity`). The separate multiplayer app is
  now only the two-players-on-one-device test copy (`-Instance 2`). Run on
  an AYN Thor: both launcher entries, and the hand-off in both directions
  ([docs/MW3-MP-BRINGUP.md](docs/MW3-MP-BRINGUP.md#one-app-two-libraries)).
- **GPUs without subgroup ballot in pixel shaders** (the Retroid Pocket
  Flip 2's Adreno 650 under Turnip) draw the game instead of running it with
  a black screen: there a pixel shader reads its own draw record and indexes
  its textures non-uniformly, the form before the waterfall. Devices with
  ballot keep the waterfall; the log names the form at start
  ([docs/shaders.md](docs/shaders.md)).
- **Adreno 6xx (Retroid Pocket Flip 2): a level no longer resets the
  device, and Modern Warfare 3 holds 30 frames a second there.** Turnip is
  started without image compression (`TU_DEBUG=noubwc`), which hard-reset
  the device as a level's world was first drawn; the FPS cap starts at 30
  and the sun's shadows off (`MW2_SHADOWS`), the two things that turn 25
  uneven frames a second into 30 even ones
  ([docs/android.md](docs/android.md#adreno-6xx-snapdragon-865-class)).
- **`MW2_FPS_LIMIT=30` is the console's pacing at half rate**: every frame
  on screen for two blanks of a 60 Hz display, instead of the title's
  limiter, whose 33 ms frames showed for one blank or three every second or
  so ([docs/switches.md](docs/switches.md)).
- **MW3 multiplayer: bots** (`MW2_BOTS=<n>`, `MW2_BOT_SKILL`, `MW2_BOT_TEAMS`,
  `runtime/bots.cpp`; the app's "Bots in my matches"): the host's server adds
  computer players through the test-client path the executable kept, they
  choose a team and a class through the stock menus, shoot at the enemies
  they see and walk towards the nearest one. Host only; a bot leaves when a
  player needs its slot. `bots <n>`, `bots +`, `bots -` on the host's console
  ([docs/switches.md](docs/switches.md)).
- **MW3 multiplayer: voice chat** (`MW2_VOICE=1`, `runtime/kernel/voice.cpp`;
  the app's "Voice chat", off by default): the headset the title's voice
  library asks XAM for, made of the microphone (SDL, or AAudio on Android)
  and the game's sound output. Push-to-talk by default (T, or the on-screen
  Talk button); the Android app asks for the microphone only when voice chat
  is turned on. Speech passes through the relay unencrypted. The earpiece is
  8 kHz, and a profile with no stored voice volume reads as 100, where it
  read as 0 and silenced every other player
  ([docs/multiplayer.md](docs/multiplayer.md#voice-chat)).
- **One-command Windows setup** (`setup/setup.cmd`, `setup/setup.ps1`): fetches
  pinned, hash-checked tools into `.toolchain/`, asks for the player's own disc
  image, checks its executables (`tools/check_mw3_xex.py`), recompiles them and
  builds the Android apps and/or the Windows game, then optionally installs
  them over adb; `-Check` reports prerequisites without changing anything.
  The README is rewritten for players (status, quick start, online play,
  credits, disclaimer); `THIRD_PARTY.md` lists every component's licence.
- **Skin tools** (`tools/iwi_match.py`, `tools/camo_gen.py`): match texture
  dumps to the user's own PC `.iwi` files and write the user's modded images
  under the dumps' keys; draw original procedural camos (carbon, voronoi,
  marble, digital, iridescent, plasma, animated variants)
  ([docs/textures.md](docs/textures.md#making-skins)).
- **MW3 multiplayer: unlock everything** (`MW2_UNLOCK_ALL=1`,
  `runtime/mw3_unlock_all.cpp`; the app's "Unlock all camos and items", on by
  default): the title's unlock checks answer yes, in the menus and where a
  match checks a spawning player's class, so a chosen camouflage is on the
  weapon in play; the stats are not changed
  ([docs/MW3-MP-BRINGUP.md](docs/MW3-MP-BRINGUP.md#everything-unlocked)).
- **Modern Warfare 3's multiplayer executable builds as a title of its own**
  (`MW2_TITLE=mw3mp`, `config/MW3MP.toml`, `runtime/title.h`, the `mw3mp`
  Android flavour, `com.mw3.recomp.mp`): recompiled, linked and packaged, not
  yet run, and without a network
  ([docs/MW3-MP-BRINGUP.md](docs/MW3-MP-BRINGUP.md)). The recompiler patch
  gained the nine VMX halfword instructions that executable uses.
- **MW3 multiplayer network hardening** (`runtime/title.h`, the fix files of
  [docs/security.md](docs/security.md#modern-warfare-3-multiplayer)): MW2's
  multiplayer fixes located in `iw5mp_ship.exe` and wired, three shown absent
  in IW5, and two IW5-only fixes (`runtime/partystate_fragment_index.cpp`, the
  snapshot glass state in `runtime/glass_piece_count.cpp`). Compiled, not yet
  run; the title's network stays off.
- **Texture replacement (skins)** (`runtime/gpu/texture_replace.cpp`,
  `runtime/png.cpp`, `runtime/gpu/vulkan/texture_cache.cpp`): a PNG or PNM
  named by a texture's content key, in `replace/` in the game folder or
  `MW2_TEXTURE_REPLACE=<dir>`, is uploaded in that texture's place.
  `MW2_DUMP_TEXTURES=<dir>` now works in every build and names its files by
  that key, as PNG ([docs/textures.md](docs/textures.md#texture-replacement-skins)).

### Fixed
- **Walls no longer stay undrawn on a first visit** (`runtime/gpu/vulkan/renderer_pipelines.cpp`,
  `PipelineWhenReady`): with pipelines compiled in the background
  (`MW2_ASYNC_PIPELINES`, the Android app's default) a draw was skipped for as
  long as its pipeline took to reach the front of the worker's queue, and the
  phones' drivers are given one creation at a time. On a Retroid Pocket Flip 2
  (Adreno 650) with empty caches a first Seatown match skipped 298,275 draws
  and left walls, ground and props out for up to 288 frames, ten seconds of
  seeing through them. A draw now waits 20 frames at most, and then the
  consumer builds its pipeline itself: the same match skips 37,790 draws, none
  for longer than 20 frames, at the cost of one long frame as a place is first
  seen (4.4 s at the first spawn there). With the caches a run leaves behind
  nothing is built at a draw and the match holds 30.0 frames a second.
- **The Windows build holds the mouse** (`runtime/gpu/vulkan/presenter.cpp`,
  `runtime/kernel/input.cpp`): the cursor used to walk out of the window as
  the player looked round, and stop turning the view at the screen's edge.
  The window now takes the mouse in SDL's relative mode while it has the
  keyboard; `Alt+Tab` or `F2` lets it go. Mouse look is a turn of so many
  degrees a count (`MW2_MOUSE_SENSITIVITY`) whatever the frame rate, and a
  slow hand is no longer lost in the stick's dead zone
  ([docs/runtime.md](docs/runtime.md#input)).

### Changed
- **Consecutive draws that change no state are one hardware draw**
  (`runtime/gpu/vulkan/renderer_draw.cpp`, `renderer_arena.cpp`,
  `shader_translator.cpp`): the GPU's cost on Adreno is per hardware draw, and
  an entry of a `vkCmdDrawMultiIndexedEXT` was one. A run of indexed draws is
  now one `vkCmdDrawIndexed` over 32-bit indices that name each draw's entry in
  a draw table, through which a vertex finds its draw's record
  ([docs/rendering.md](docs/rendering.md#the-recorder-thread)). The renderer
  now needs non-uniform indexing of sampled images as well as subgroup ballot
  in pixel shaders. `MW2_NO_MERGE=1` makes every draw a hardware draw of its
  own. Cached shaders of earlier builds are not reused.
- **A pixel shader runs once for each draw its subgroup holds**
  (`shader_translator.cpp`, [docs/shaders.md](docs/shaders.md#the-module-interface)):
  merged draws had made the record, and with it every constant's address and
  texture index, a value per pixel, which nearly doubled the pixel work of a
  frame on Adreno. The program is now the body of a loop that takes one
  record a pass, so they are one value for the subgroup again.

## [v0.0.2-alpha] - 2026-10-18

### Highlights
- **Release tags with a suffix are now read.** `v0.0.2-alpha` was a tag the
  launcher's update check could not parse at all, so it answered "its
  description is not one" and no player was ever offered it — as every
  `v0.1.0-alpha.N` release before this one went unoffered.
- The apk says which release it is, instead of "1.0".
- Faster software decoding of block-compressed textures on Mali and Adreno
  (BC2 10.1 → 4.7 ms per 2048x2048 decode).

### Added
- **`launcher/version.h`**: release tags compared as semantic versioning orders
  them — `0.0.2-alpha < 0.0.2-beta < 0.0.2 < 0.0.3-alpha`, with `alpha.10` after
  `alpha.9` and not before it, and a tag that is not a version refused rather
  than ordered somewhere. `update::Look` uses it (`launcher/update.cpp`).
- **`tests/version_test.cpp`** (`ctest` name `version`): the order above, the
  project's own published tags, and the shapes that are not versions.
- **`tools/bc_bench.cpp`**, built as `bc-bench`: how long one 2048x2048
  software BC decode takes per format. The figures quoted in
  `runtime/gpu/vulkan/bc_decoder.cpp` come from it.
- **`MW2_VERSION_NAME`** (and `MW2_VERSION_CODE`) in `android/app/build.gradle.kts`:
  the release the apk was built from, taken from the tag by `build-apk.yml`.

### Changed
- **BC decoder, row at a time** (`runtime/gpu/vulkan/bc_decoder.cpp`): BC2, BC3
  and BC5 walked all sixteen pixels of a block with a computed x, y and shift;
  they now take a row from a word already loaded, with the block's masked
  colours and lifted alphas made once. On a 2048x2048 decode (best of 12,
  `bc-bench`): BC2 10.1 → 4.7 ms, BC3 7.1 → 6.4 ms, BC5 6.7 → 6.4 ms, BC1
  unchanged at 3.8 ms. The output is byte for byte what it was —
  `tests/bc_decoder_test.cpp` compares it against a decoder written from the
  formats themselves.
- **`versionCode` 15 → 16**, so Android installs this over v0.0.2.
- **`.github/workflows/release.yml`**: a tag that is not a version now fails the
  run before anything is published — a release tag the launcher cannot read is
  a release nobody is offered — and a tag with a suffix after its version is
  drafted as a pre-release, which is what keeps GitHub's "latest release" (the
  one the launcher asks for) on a version everyone can install.
- **`docs/building.md`**: the list of harnesses and `ctest`'s checks, which said
  three targets where there were four and now are five.

### Fixed
- **A build with no release tag no longer compares itself against the newest
  release.** The empty version parsed as no numbers, which every version sorts
  above, so an unreleased build was offered an update it should not have been
  (`update.h` says such a build does not update; now it does not).

### Notes
- GitHub's "latest release" — the endpoint the launcher reads — is never a
  pre-release, so publishing `v0.0.2-alpha` as a pre-release reaches players
  through the releases page and the apk, not through the in-launcher updater.
  That is the intended behaviour for an alpha; a pre-release build being
  *offered* pre-releases would need the list endpoint instead.

---

## [v0.0.2] - 2026-10-13

### Highlights
- Comprehensive fixes for Qualcomm Snapdragon (Adreno) chips.
- Compatibility floor lowered to **Vulkan 1.0**.
- ARM64 CPU thread affinity for Prime (Cortex-X) cores.
- TBDR mobile tile rendering optimizations.
- Virtual Filesystem (VFS) mod directory overlay (`mods/`).
- Ingestion security hardening for disc extraction and archives.

### Added
- **ARM64 CPU Affinity**: Pinned main game loop to the Prime Core (Cortex-X), command recorder to Performance Cores, and background worker to Efficiency Cores via Bionic `sched_setaffinity` (`runtime/platform.cpp`).
- **Mod Directory Overlay**: Added support for offline Bot Warfare GSC mods and loose assets located in `MW2_MOD_DIR` or `<GameRoot>/mods/` (`runtime/kernel/files.cpp`).
- **Pillar Architecture Documentation**: Created technical specifications for CPU affinity, Vulkan mobile pipeline, memory budgeting, and bots/netcode in `docs/pillars/`.
- **In-Game FOV Control**: Merged upstream FOV slider support and camera adjustments.

### Fixed
- **Snapdragon / Adreno BC Texture Bug**:
  - Automatically routes BC1–BC5 textures through SIMD software decoding on Qualcomm GPUs (`vendorID == 0x5143`) to avoid hardware 1-bit alpha sampling defects.
  - Representation-safe texture uploads and restaging in `runtime/gpu/vulkan/texture_cache.cpp`: decompression failures are explicitly caught and reported, preventing black terrain, flashing vegetation, and black skybox artifacts.
- **Vulkan 1.0 Support**:
  - Manifest hardware requirement lowered to `0x400000` (`android/app/src/main/AndroidManifest.xml`).
  - Dynamic capability detection and fallback to classic Vulkan 1.0 render passes and negative viewport extensions.
- **TBDR Mobile RenderPass Optimization**:
  - Configured `VK_ATTACHMENT_STORE_OP_DONT_CARE` on stencil and depth targets in `runtime/gpu/vulkan/renderer_targets.cpp`, eliminating tile memory spilling.
- **Archive Extraction Hardening**:
  - Fixed order-of-operations bug in `tools/fetch_asset.py` where temporary file was deleted before reading header bytes.
  - Implemented zip-bomb bounds (50,000 entries and 15 GiB limit).
- **XDVDFS Disc Extraction Hardening**:
  - Added sector bounds validation against total image size (`tools/xdvdfs.py`).
  - Added directory cycle detection and recursion depth limits.
  - Enforced destination path containment using `os.path.realpath` to prevent path traversal.
  - Added truncated read validation.
- **CI & Release Determinism**:
  - Made manual APK builds artifact-only by default and enforced explicit release tag validation (`.github/workflows/build-apk.yml`).

---

## [v0.0.1] - 2026-10-10

### Added
- Initial release of the native Modern Warfare 2 recompilation port for Android ARM64 (`com.mw2.recomp.sp`).
- XenonRecomp static PowerPC recompiler generating 64-bit ARM machine code (`libmw2.so`).
- Vulkan renderer backend supporting mobile TBDR architectures.
- Low-latency AAudio native sound sink at 48 kHz stereo.
- Integrated Android launcher with on-screen touch controls, gamepad support, and XDVDFS ISO installer.
- Custom GPU driver installer (`libadrenotools`) in `DriverStore.kt`.
