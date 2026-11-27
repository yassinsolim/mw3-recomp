# Black Ops III (Xbox 360, title 4156091D): bring-up log

Titles `bo3` (default.xex, the multiplayer) and `bo3zm` (default_zm.xex, the
zombies); the Xbox 360 game has no campaign. Windows, and a first boot on
Android. Every state below says how it was seen; what was not seen is listed
as open.

## The files

- The disc has only `core_*` fastfiles (version 595). The multiplayer, zombies
  and front-end zones are in title update 5 and Compatibility Pack 1 (version
  606), which the disc's executables refuse.
- The update's `default.xexp` and `default_zm.xexp` apply to the disc's
  executables (0.0.0.5 to 0.0.8.5, XenonRecomp's patcher), and the two titles
  are those patched executables (`bo3/tu5/`, not in the repository).
- The player's packages are read where they lie (`MW2_DLC_DIR`, or `dlc`
  beside the game): the update is mounted as `update:`, the others as the
  title asks for them.

## Recompiling

50,989 functions (multiplayer) and 50,747 (zombies), 168 jump tables each
(161 from XenonAnalyse, 7 from `tools/intext_switch_tables.py`), 61 function
extents. `tools/extra_instructions.py` writes 1,006 hooks for each: nineteen
instructions the pinned recompiler has no case for, and two it writes without
their dot, `rldicl.` (6) and `rlwimi.` (2), which must set cr0. The Lua
interpreter tests a boolean with `rldicl.`, so every `false` read as true and
the menus asked for a module only a campaign would have
(`T6.Menus.Bracket`). No earlier title has either instruction (their images
were scanned), so their configurations and game-library statements are as
they were.

## What the kernel had to learn

- A licence of bit 0 alone is told to the title as every bit. Treyarch's
  engine takes it for a trial that counts only signed in to Xbox Live
  (sub_826528F8), and Compatibility Pack 1, The Giant and Awakening are
  licensed so: the title asked to download Compatibility Pack 1. The reading
  is Black Ops II's port's (spartansprm/BO2-Android-xbox360,
  `android/game/dlc_license.h`, GPL-3.0); no code was copied.
- A package's folders are listed (ReadVolume::Children). The title walks each
  package folder by folder and finds a sound bank by its name and the folders
  above it; with the root's files alone it read the update's banks from the
  disc and gave up (`snd_bank.cpp:1352`).
- The renderer translates `getWeights` (41 of the 263 pixel shaders seen).

## Milestones on Windows (RTX 5080, 1280x720)

| State | How it was seen |
|---|---|
| `Press A to Start`, no Lua error | frames, log |
| Brightness, Safe Area, the main menu (Multiplayer, Freerun, Zombies, Find LAN Game, Play Online) | frames |
| Multiplayer's local lobby, Combine, Team Deathmatch, the countdown, the loading screen, Choose Class | frames |
| In the match: the HUD and the map, the pad moves and turns the player, the magazine counts down as the trigger is held (30, 24, 9) | frames |
| `bo3zm`: the zombies' lobby (Shadows of Evil), the match starts, 500 points, `Game Over, You Survived 1 Round` with nobody playing | frames |
| The world | drawn in both titles (frames): a resolve fetched as a tiled volume, a depth and a colour copy at one address, a depth resolve running on into the next surface, colour format 10, and the streamed images' moves |
| Multiplayer with bots (Setup Bots, 3 a side, Combine) | frames and the scoreboard: the bots kill each other, the scripted player moves, fires, reloads and dies; 58 to 60 frames a second |
| Find LAN Game between two copies on one PC (`MW2_NETWORK=lan`) | frames: the host's lobby is listed, joined (2 Players in both lobbies) and one match is played by both |
| `bo3zm`: Shadows of Evil to `Game Over` and the After Action Report | frames; the scripted player kills nothing, so round 1 only |
| `bo3zm`: Shadows of Evil past round 1 | `Game Over, You Survived 2 Rounds`, 8 kills with the knife, 1,560 points (frames, the test drive below) |
| `bo3zm`: The Giant, from the owner's pack read where it lies | Change Map lists Shadows of Evil, The Giant and Der Eisendrache; The Giant's opening film plays (about 150 s, or `A` skips it), the match starts at the mainframe and ends in `Game Over` in round 1 (frames) |

The runs were driven by `MW2_INPUT_SCRIPT="20:a,28:a,36:a,46:a,56:a,84:a,90:a"`
(the zombies' `36:a` is Start Game; `35:down,36.5:a,40:down,42:a,46:up,47.5:a`
in its place picks The Giant). About 42 frames a second in the match, with
the diagnostics on.

**The zombies' test drive.** Fixed times cannot aim, and a player who stands
still is down within 35 seconds of the first round. The drive reads the
dumped frames while the game runs and writes `MW2_INPUT_LIVE` lines: it
waits for the HUD, turns the view by the mouse (`turn1000/0:1`, three times),
then keeps what is green in the frame in the middle (the doorway the first
zombies come through) and uses the knife and the pistol twice a second. A
turn of the mouse is not the same angle at every frame rate, which is why
it steers by the picture. It is not in the repository (it needs the frames
of a diagnostics build and this PC's paths).

## The Vulkan validation layer, and the GPU resets (26 November 2026)

A multiplayer match and a zombies match were run under
`VK_LAYER_KHRONOS_validation` 1.4.363 with GPU-assisted validation
(`VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT`), 200
seconds each, one copy alone.

- Two pixel shaders were not valid SPIR-V: a texture slot fetched both as a
  volume and along two axes (`getWeights` of a volume) took the last fetch's
  dimension, and the other fetch asked the image's size in two components.
  A slot any fetch reads as a volume is a volume now; the layer has nothing
  to say of the shaders since.
- Once in a run, at a loading screen: `vkQueueSubmit` signals a semaphore
  the swapchain may still be using (the presenter has a semaphore for each
  frame slot, not for each image). It is the same for every title and was
  left as it is.
- Nothing else: no error of GPU-assisted validation in either match.

Windows logged no NVIDIA reset (nvlddmkm 153) in any of some forty runs that
night: under validation, without it, two copies at once, and one that
presented four frames a second for its whole length. That last state is what
preceded the resets of the evening before, and it is not a hang: the log
says `the window took no frame in 250 ms`, which is the window covered by
other windows (three other agents' games were open). The resets of the
evening before came with up to nine game copies of four agents on the card,
and were not reproduced with this title alone.

## The freeze in the zombies (gpu/memory_watch.cpp)

`Loosen` left a page Loose and read-only when a writer was put aside between
making it writable and marking it (the page was armed again in between):
every write then faulted for ever. The zombies stopped there about twenty
seconds into a match in 2 runs of 6 (a worker spinning on one store in
sub_8247C450, the main thread waiting for its job; lldb, and a diagnostic
that found the page Loose with `PAGE_READONLY` after 20,000 faults and let
the match go on once it made the page writable). A Loose page is made
writable at its next fault. Thirteen zombies matches and five multiplayer
matches since, on Windows and on the Thor, none stopped. The change is in
`android` by itself (7a1973a), after a run of each of the four games with
it on an AYN Thor.

## Android (26 November 2026)

The `bo3` flavour (`com.bo3.recomp`, an orange emblem with a diamond):
`libbo3.so` is the multiplayer and has the place the other apps' campaign
has, `libbo3zm.so` is the zombies and has their multiplayer's. The game
package holds `libbo3game.so` and `libbo3zmgame.so`
(`config/abi/bo3`, `config/abi/bo3zm`: the statement is the code's hash).

**One set of game code, from the disc and title update 5.** The game folder
has the disc's files with the disc's two executables, and the `dlc` folder
beside it the player's packages as the console keeps them: the title update,
Compatibility Pack 1 and the map packs. At every start the runtime applies
the update's `default.xexp` (`default_zm.xexp`) to the disc's executable in
memory and runs it when its image is the one that was recompiled
(`runtime/install/install.cpp`, as Advanced Warfare's updated multiplayer).
The packs are only read. Nothing is built for a pack.

| State (AYN Thor, debug app `com.bo3.recomp.bo3`, the game libraries built on the PC) | How it was seen |
|---|---|
| `Press A to Start`, the main menu, the local lobby, Choose Class | screenshots; 57 to 60 frames a second |
| A multiplayer match on Combine, the scripted player walking and firing | screenshots; 42.7 frames a second over 169 s of play (45 to 57 on the overlay) |
| Shadows of Evil to `Game Over` | screenshots; 56.5 frames a second over 64 s of play |

**With no PC (26 November 2026).** The app's steps are three, in this
order, and each was done on an AYN Thor through the app's own screens (taps
over adb, screenshots):

1. *Install & Play* finds the disc image and sets the game up to play from
   it: the two executables and the picture are copied, a few seconds. The
   disc's sound banks are two folders down (`snd\en\<bank>`), so the image's
   volume holds its deeper files and lists its folders for this title
   (`kernel/volume.cpp`). Copied instead, the installer takes the `.xpak`
   files, `video\` and `snd\<language>\` as well (`install/install.cpp`; the
   copy was not run on a device).
2. *Add DLC*, the folder `4156091D` chosen in the system's picker: title
   update 5 and Compatibility Pack 1 (2.0 GB) were in the dlc folder ten
   seconds later, and the build started by itself.
3. The build: both libraries from the image and the update, the statements
   the PC's (`InTextTables.kt`'s tables and the patched executables are the
   script's and XenonRecomp's to the byte).

| | AYN Thor (Snapdragon 8 Gen 2, 16 GB) | Retroid Pocket Flip 2 (Snapdragon 865, 8 GB) |
|---|---|---|
| recompile, each title | 36 s | 47 s |
| compile, each title (201 and 202 files) | 226 and 229 s | 378 and 368 s |
| the whole build | 529 s | 842 s |
| compilers at once, least free memory | 8, 10.3 GB | 8, 4.0 GB |
| hottest battery | 31.0 C | 33.2 C |

The Flip had no room for the image (8.7 GB): its game files and packages
were pushed over adb, and only the build went through the app there. Both
devices had toolchain package 1; package 2, which this app's kit names, was
installed over adb for the runs and package 1 put back after.

**Compatibility Pack 2 is needed too.** With the update and Pack 1 alone
the multiplayer says `In order to play multiplayer, you need to download
new required content`; with Pack 2 added it plays. The app counts both as
missing until they are there (`Dlc.kt`).

**A thread's data.** The image says a thread has 8,568 bytes of its own
(the earlier titles' say 36 to 56); the runtime gave each 512 and did not
copy the image's beginning of them. The rest lay in whatever the heap gave
next, another thread's block among it. The engine keeps there how many
times a thread has entered each of its critical sections, and enters only
at zero (sub_82651B80): from section 106 on the count was outside the
block. `kernel::CreateThreadBlock` sizes the block by the image now. This
is the likeliest cause of the errors listed under Open as "reading memory
another thread has not finished with", and of `Exceeded limit of 80
'physpreset' assets` on the Thor: a match uses 32 of the 80 (read from the
pool on Windows), and the error is raised when the pool's list of free
entries is empty, not at a count. It was not seen again in the three device
matches since, one of them before the change, so that is not shown; the
error now logs each pool with no free entry and its count
(`engine_log.cpp`), which tells a list that lost entries from a pool that
ran out.

**Frames a second on the bundled Turnip driver** (`driver:
libvulkan_freedreno.so (adrenotools)` in every log; the scripted player
walking and firing; game code built on a PC, the statement the device's;
`MW2_REPORT`, every run recorded with `adb screenrecord`). The figures of
the rounds before were Qualcomm's own driver's: the worktree had neither
the Turnip package nor libadrenotools (`android/fetch_deps.sh`).

| | in play | the chip's frame | the chip at work |
|---|---|---|---|
| Thor, multiplayer on Combine, first run and second | 55.6, 56.5 | 5.5 ms | 31 to 35% |
| Thor, Shadows of Evil | 56.7 | 6.7 ms | 38% |
| Flip, multiplayer on Combine | 55.1 to 55.7 | 14.2 to 15.3 ms | 79 to 84% |
| Flip, The Giant | 58.4 (26.2 before the loops below) | 15.5 ms (37.5) | 91% |
| Flip, Shadows of Evil, before the crossing jumps below | 23 to 33 | 31 to 44 ms | 98% |
| Flip, Shadows of Evil, with the 30 dispatcher shaders left out | 58.4 | 13.4 ms | |

The Thor is not held by its chip: the title's own 60 a second is what it
runs at. The Flip's scene is at 70% of its width and height nearly all the
time (`MW2_SCENE_DYNAMIC`), and the Thor's is drawn smaller too (76 to 92%
in half its frames) though its chip is idle two thirds of the time: the
rule that shrinks it measures against the 120 Hz display there, which is
open.

Two things were the zombies' frame on the Flip, both the shader
translator's dispatcher ([shaders.md](shaders.md#backward-jumps-as-loops)):
a glow in the HUD that loops 121 times a pixel (32 ms a frame in The Giant),
and the lit surfaces' `if`/`else` (26 ms in Shadows of Evil). Both are
emitted as loops and selections now. Shadows of Evil and The Giant were not
measured again on either device after the second change and the merge of
`android` (the Thor's 56.7 is with both).

**Add DLC on the Thor, through the app** (26 November, taps over adb):
Install & Play found the image in a folder of Download and set the game up
in six seconds; Add DLC, the folder `4156091D` chosen in the system's
picker, copied title update 5, Compatibility Packs 1 and 2, The Giant and
Awakening (4.0 GB) in under five seconds and named each. The merge of
`android` had Modern Warfare 3's rule for packs (a `dlc<n>.id` marker)
applied to this title, which then used none of its packs and asked for its
required content; `kernel/content.cpp` leaves it out.

**The frames** (`tools/visualqa`, every frame of each recording). On the
Flip: multiplayer, second run, nothing but the player's own muzzle flashes;
its first run, with the pipelines compiled as they are drawn, one frame of
a flat wash with the HUD over it. The Giant: a panel of the teleporter drawn
with another texture for one frame, twice in two seconds, 25 s into the
match; not looked into. One multiplayer run in five on the Flip ended in a
lost device after a five second stall (before the dispatcher changes; not
seen since, in four runs).

**The round of 26 November, afternoon** (the build with `android` merged to
`821d28f`; Turnip in every log; `tools/visualqa` over every recording and a
look at each frame it named).

| | in play | |
|---|---|---|
| Flip, multiplayer on Combine, first run and second | 56.4, 56.1 | muzzle flashes only; no flat wash on the first run |
| Flip, The Giant | 57.5 | muzzle flashes and snow only, with the lighting table read at its bind (below) |
| Flip, Shadows of Evil | 57.8 | the far end of the street drawn as flat rectangles for a second as the zombies arrive: open |
| Thor, multiplayer on Combine, first run | 56.5 | the scene whole in every frame, the chip at work 33% of the time |

- **The Thor's scene kept its size** at the time: the title then made 60 a
  second at most (see "Past 60 frames a second" below, which ended that).
- **Models lit wrongly for one frame** (the teleporter's door and pad, the
  held weapon, a zombie's arm; the "panel's texture" above). The model
  lighting table, a 512x256x4 volume as Modern Warfare 2's, was read again at
  the end of each segment that binds it and found half written: four or five
  such frames in 46 s of standing still in The Giant on the Flip, none in
  three runs with the table read once, where the frame first binds it
  (`MW2_TABLE_AT_BIND`, on for this title alone then; since, every rewritten
  texture in every title is read so, `MW2_TEXTURES_AT_BIND`). Not seen on Windows
  either way.
- **The zombies' start.** The launcher's ZOMBIES entry starts the zombies'
  executable with no launch data (`TitleUpdate.zombiesLaunchData` is Advanced
  Warfare's Exo Zombies'), and the executable opens at its own lobby. Twice,
  on a Flip whose new app had only run the multiplayer, its first start
  showed the main menu instead, where A is Multiplayer; the start after was
  the lobby. Not looked into. A test's `run_request.txt` is written again by
  the launcher's direct launch, so a second line in it never reaches the
  game.
- The Thor's zombies of this round are not counted: `adb push` of the game
  folder stopped at 111 files of 380 (`copy response: EOF`), and a
  multiplayer run on it ended in `Could not load default asset`.

**The chequerboard moves (26 November, evening).** The image
pool's move function takes flags, and with bits 3 to 8 set the block is
every other 128 bytes of a 4 KB or 8 KB piece, the other 128s in the next
piece: two blocks lie in each other's gaps, and a second vertex shader
copies them a piece a draw (`image_move.cpp`). Taken for plain moves, they
matched no draw and were made whole at the end of the frame after, late and
over the other block's half: 4,517 of 22,309 moves in a hundred seconds of
Shadows of Evil on Windows, none since (`image pool:` in the log, 0 at the
end of their frame). It was the Flip's flat far street (the round below).

What it is not: the one flat frame in several thousand on Windows. That is
still there at the same rate with the moves right (2 in 12,100 frames of
one copy alone; `MW2_FLASH_LONE=6` marks them), and is not the shadow of
guest memory (seen with `MW2_NO_SHADOW=1 MW2_KEEP_VERTICES=0`), the scene's
size (seen with `MW2_SCENE_DYNAMIC=0`), a slow ring consumer or a vertex
constant that is not a number. In the marked frames looked at (about a
dozen) a thin streak or a wide flat band, the colour of the zombies' eyes
or of the fog, starts at a zombie's head, and the streak is there some
frames either side: it looks like the eyes' trail drawn from a point that
is not the zombie's. Its vertices were not caught, so that is a guess.

That a draw's vertices past its declared piece write nothing is read from
the layout (the pool's sources come in pairs 4 KB apart, each as long as
the other, which only fits if each has half), not from a console: with all
of them written the far street was flat rectangles in dozens of frames a
run, with the piece alone in none beyond the streaks above.

**The round of 26 November, night: both phones, and the merge into
`android`.** The apps are the debug builds of the branch with `android`
merged (`ae96179`: the guided setup, what a device keeps measured), but for
the first three rows, which are of the commit before that merge; Turnip in
every log; every run recorded, `tools/visualqa` over the recordings and a
look at each frame it named. Frames a second are of the seconds of the
match.

| | in play | the chip | the frames |
|---|---|---|---|
| Thor, Shadows of Evil, from the app's own install (below) | 57.6 | 6.6 ms, at work 37% | the player's own tracers only |
| Thor, The Giant | 58.6 | 8.4 ms, 49% | clean |
| Thor, multiplayer on Combine | 56.5 | 5.6 ms, 32% | muzzle flashes only |
| Thor, Shadows of Evil, the merged launcher's ZOMBIES entry | 58.7 | 6.1 ms, 35% | |
| Flip, Shadows of Evil, two runs | 58.4, 58.3 | 13.8 ms, 80% | no flat rectangle; a thin streak for one frame, once and five times (open) |
| Flip, The Giant | 58.7 | 14.7 ms, 86% | muzzle flashes and snow only |
| Flip, multiplayer on Combine (the commit before the merge), two runs | 56.0, 56.1 | 14.1 ms, 78% | |

- **The Flip's flat far street is gone.** Counted as frames unlike both
  their neighbours over more than a twentieth of the picture, in the eighty
  seconds of recording around the zombies' arrival: 100 of 4,271 before
  (`c6d16c5`), 0 of 4,569 and 0 of 4,577 now. The log's count of moves made
  late: 6,535 of 30,464 before, 0 of 36,159 and 0 of 39,668 now.
- **The Thor's install was the app's.** Install & Play found the image in a
  folder of Download and set the game up to play from it in five seconds;
  Add DLC, the folder `4156091D` chosen in the system's picker, copied the
  update and the four packages (4.0 GB) in three seconds, and Shadows of
  Evil, The Giant and a multiplayer match were played from that. Done again
  with the merged launcher, where the one large button is *Install the game
  files*, then *Add DLC*, then *PLAY*: Black Ops III keeps the install
  screen and is not in the guided setup, whose order is disc, build, packs
  (`Setup.guided`).
- **The rate's steps stopped at 60** (`adaptive.cpp`) while the title made
  60 at most; since "Past 60 frames a second" they are the other games'.
- The Flip's scene is at 58% of its width in three quarters of the frames
  (70% before the merge of `android`, whose floor is lower).

**The other four games on this build** (`tools/visualqa`, the ten
scenarios, the test apps beside the owner's). On the Thor, 117.1 to 118.9
frames a second, eight passed at once and two did not:

- `mw3-bots`: the held weapon and the arm drawn black for one frame, 82 s
  into the run; three more runs passed. A model lit by nothing for a frame
  is what this title's lighting table read half written looks like (above);
  Modern Warfare 3 read its table at the segments' ends then
  (`MW2_TABLE_AT_BIND` was off for it; since, `MW2_TEXTURES_AT_BIND` reads it where the frame first binds it), and no run of `android` by itself was
  made to say whether it is seen there.
- `aw-shadows`: the run ended 57 s in, while Solar loaded. The debug
  build's dump of every thread's stack at a `STALL` (`crash.cpp`) faulted
  inside the unwinder (`SIGILL` at an `autia1716` of libunwind's
  `stepWithDwarf`); the run after passed at 118.5. Not this branch's code:
  an app built from `android` alone (`8b7d1a6`) ended the same way in one
  run of three. Not in a release build, which has no such dump.

**Again with `android` at `8b7d1a6`** (the frame slots' kept vertex data,
Advanced Warfare's field of view; the branch at `6ddc0ad`), the ten
scenarios on each phone and Shadows of Evil on the Flip:

- The Flip: ten of ten, 59.7 to 60.4. Shadows of Evil: no flat rectangle,
  no move made late, and the thin streak in nine single frames of a match
  of 4,400.
- The Thor: eight of ten, 117.3 to 118.7, `mw3-bots` among them. The two:
  `aw-campaign`, the whole frame washed bright for one frame in the drop
  pod, 94.6 s into the run, in one run of four (a run of another branch
  without this one's changes had the same at 93.0 s that day); and
  `aw-shadows`, three runs of three, each a single frame late in the walk
  where a door post or a wall's edge crosses the left of the picture in a
  fast turn, or the view passes a dark doorway: the check's own mistake,
  the picture whole in each. The walk is wider since that merge
  (`MW2_FOV=85` where the scenario had set `cg_fov`), and the one run of
  `android` alone that finished passed.

Open on Android:

- A thin streak, white or yellow, from a zombie's head across the picture
  for one frame: one, five and nine such frames in three Shadows of Evil
  matches of 3,400 to 4,400 frames on the Flip, two in 12,100 frames on
  Windows (above).
- The copied install was not run on a device.
- Setup on a PC (`setup.ps1 -Game bo3`) does not exist.

## Past 60 frames a second (27 November 2026)

**What held it at 60.** Not the present interval (the runtime asks D3D for
an immediate flip, as for the other games) and not the guest's blank (a
blank at 120 Hz left a match at 60.0): the title's vsync dvar, a bool whose
pointer is at 0x83820150 (zombies 0x83858360). The swap callback the title
gives D3D (sub_824A2848, D3DDevice_SetSwapCallback's one caller) sets each
flip's blank to the last one's plus one while it is on, and the render
thread waits for the flip before the next frame (sub_82478270 polling
D3D's pending-swap count), so frames came at the guest's 60 whatever the
limit. Its com_maxfps is read by Com_Frame (sub_8264DFD0) only while vsync
is on. The console cannot be asked for either (the dvars go by hashes), so
the runtime writes the dvar's value at every present while the title is
off the console's pacing and puts the title's own back on it
(`d3d_hooks.cpp`, `TitleVsync`, `T_DATA_VsyncDvar`), and holds a limit under
the display's rate itself (`HoldToLimit`, as Call of Duty 4's). The 60 caps
in `adaptive.cpp`'s steps and the scene rule's frame time are gone.

**Windows** (RTX 5080, 240 Hz, a match on Combine, frames counted between
dumps 10 s apart): no limit 60.0 before, 237 with the dvar off; a limit of
120 holds 119.4 to 119.5; 60 is the console's 59.6 to 59.9.

**The game's timing at 120 against 60** (Windows, the same build, the
scripted player):

| | 60 | 120 |
|---|---|---|
| match clock, 70 s of wall time | | 9:40 to 8:30, 70 s |
| server frame (sv thread, sub_8267F438), multiplayer | 50.1 ms | 50.0 ms |
| server frame, Shadows of Evil | 50.0 ms | 50.0 ms |
| KN-44, trigger held 2 s | 21 rounds, 10.0 a second | 21 rounds, 10.3 a second |
| reload, press to a full magazine | 1.50 to 1.77 s | 1.37 to 1.64 s |
| walk 1 s forward, back, left, right (the minimap's shift) | 14, 15, 15, 14 px | 15, 14, 14, 14 px |
| mixer frames, xma errors (no sound device) | 25,085, 0 | 25,082, 0 |

The rates and times agree within a frame dump's 0.27 s; the server, which
runs the zombies, the rounds and the scripts, keeps its 20 frames a second
on its own thread at either rate. No `Com_Error` or `[E]` in the runs.

**AYN Thor** (debug app, Turnip, `MW2_REPORT_SECONDS`, the mean of the
seconds of play after the first ten, the slowest second in brackets):

| | limit 60 (before) | limit 120 |
|---|---|---|
| Shadows of Evil | 59.0 (51) | 105.7 (60) |
| The Giant | | 107.0 (62) |
| multiplayer, Combine | | 110.6 (70) |

On the Retroid Pocket Flip 2 (60 Hz) the limit is 60 and the pacing the
console's, as before: Shadows of Evil 57.5 (43) on a first run that compiled
its pipelines in play and 59.4 (50) on the second (58.3 and 58.4 in the
round of 26 November). At 120 on the Thor the processor is what the late
frames wait for (19 to 63% of the time; the chip 4.6 to 6.0 ms a frame).
Open: the scene rule draws the scene smaller where the processor is late
(The Giant at 58 to 70% of its width, Shadows of Evil 76 to 100%), which
does not help a frame the chip is not late for; a second multiplayer map
and the Thor's 1% low over single frames were not measured.

## Every mode and map (27 November 2026)

**The console's queue.** `Cbuf_AddText` is found in both executables
(multiplayer sub_8263A6A0, zombies sub_82631630: the "p<digit> " prefix
parse, then the client's buffer; `title.h`), so `MW2_CONSOLE` and the app's
console reach the title, and `MW2_TRACE_CBUF` shows what the menus queue.
Commands work; dvars work by name where the title registered them so
(`cg_drawFPS 1` drew the counter). The local lobby's Start Game queues
`lobbyLaunchGame 1 <map> <type>`, which ignores its arguments and takes
`ui_mapname` and `ui_gametype`: the runs below set those after the lobby's
`exec gamedata/configs/mp/mp_default_gamesettings.cfg` and before the launch.
A guest does not get a map set that way (it loads the lobby's own and drops,
`Script file not found: scripts/mp/<map>.gsc`), so the two-copy runs keep
the lobby's map. `map <name>` from the main menu fails (`mem_pmem.cpp:645`).

**Looking by touch.** `AimAssist_UpdateGamePadInput` is sub_821634E8
(zombies sub_82163388): it copies the input's pitch at +8 and yaw at +20 to
the output before anything else, as Modern Warfare 3's does. The local
client is a byte at +52 (`T_AIM_INPUT_LOCAL_CLIENT_BYTE`). The turn rates
(sub_821614E0) are multiplied by state + 168, tan(half the view's height)
over 0.75 * tan(32.5 degrees). With these, `mouse_look.cpp` turns the view
by a finger's angle in BO3 too, where before it fell back to the stick.
Measured on both phones (below): every swipe turned the view by what it
asked for at the default 13 degrees a centimetre.

**What the packages hold** (`tools/stfs.py`): Compatibility Pack 1 has the
twelve base maps, the four Freerun courses and Shadows of Evil; The Giant is
zm_factory; Awakening has mp_crucible (Gauntlet), mp_rise, mp_skyjacked and
mp_waterpark, plus zm_castle (Der Eisendrache). Eclipse (Zetsubou No Shima),
Descent (Gorod Krovi) and Salvation (Revelations) are not on this PC:
untested. The 360's zombies lobby offers only the classic mode (Start Game,
Change Map, GobbleGum, Weapon Kits), so there is no Nightmares to test.

**Windows** (RTX 5080, diagnostics build of `bo3-modes`, a fresh profile
each run, frames dumped and looked at, 150 s runs, frames a second of play):

| | result |
|---|---|
| Team Deathmatch on all 16 multiplayer maps: mp_apartments, biodome, chinatown, ethiopia, havoc, infection, metro, redwood, sector, spire, stronghold, veiled; Awakening's crucible, rise, skyjacked, waterpark | pass: each loads and plays, no `Com_Error`, 45 to 60 (two copies at once) |
| Every mode on Combine: FFA, Domination, Hardpoint, S&D, Demolition, CTF, Kill Confirmed, Uplink, Safeguard, Gun Game, Sticks and Stones | pass: each loading screen names the mode, the match runs, 58.5 to 59.3 |
| `clean` (Fracture), `prop` (Prop Hunt) | not on the 360: `Script file not found: scripts/mp/gametypes/<type>.gsc`; the menus don't offer them |
| Bots (bot_maxAllies 5, bot_maxAxis 6), Domination on Aquarium | pass: the player is killed by a bot (killcam), 53.1 |
| Freerun: mp_freerun_01 to 04 | pass: each course loads at its start, 58.7 to 59.3 |
| Zombies: Shadows of Evil, The Giant, Der Eisendrache | pass: each map's match plays to `Game Over` in round 1 (the scripted player doesn't fight), 56.0, 57.2, 59.0 |
| Find LAN Game, two copies (`MW2_NETWORK=lan`) | pass: the guest joins the host's lobby (2 Players) and plays the match on Combine |
| The relay, two copies (`MW2_NETWORK=server`, mw2-relay built from `relay/` in WSL) | pass: Find LAN Game through the relay, 2 Players, the match on Combine, until the idle guest is kicked for inactivity |
| Play Online through the relay | untested past the menu: the relay welcomes the copy, but no BO3 playlist or Find Game exists |
| The Sun shadows switch | does nothing: `sm_enable 0` and 13 other likely names change no frame; the app hides the row for BO3 |

**Phones** (debug app `com.bo3.recomp.bo3m` built from `android` at
`4c809bd`, Turnip, the default Automatic setting; the game files pushed
into a test folder whose `saves/` and `mp/saves/` were deleted before each
run; frames a second of play from the log's report; 1% low = the mean rate
of the slowest 1% of single world frames from `MW2_FRAME_LOG`, over the
match: from five seconds after its last loading stall to its end screen;
in brackets, over the whole run from five seconds after the first world
frame, menus and loading included, as in the first rows):

| | AYN Thor (120 Hz) | Retroid Pocket Flip 2 (60 Hz) |
|---|---|---|
| Multiplayer, base map | mp_spire TDM: 110.7, 1% low not logged. Combine TDM (standing, the touch run): 112.6, 1% low 27.6 (20.5) | Combine TDM: 56.5, 1% low (13.1); again in the touch run: 56.8, 15.7 (13.3) |
| Multiplayer, Awakening | mp_rise Domination: 110.9, 1% low 25.7 (19.2), the slowest 1% of frames from 27 ms | mp_rise Domination: 57.1 |
| Shadows of Evil | 105.3, 102.4 and 104.8 in three matches; 1% low (16.7 to 17.3) | 56.5, 58.1 and 59.1; 1% low (9.0 to 12.8) |
| The Giant | pass, to Game Over in round 1: 109.1, 1% low 20.9 (17.7) | pass, to Game Over in round 1: 58.1, 1% low 12.7 (12.7) |
| Der Eisendrache | pass, to Game Over in round 1: 102.7, 1% low 20.8 (18.9) | pass, to Game Over in round 1: 57.6, 1% low 18.0 (13.2) |
| Freerun | mp_freerun_01: pass, the course's clock runs: 110.5, 1% low 17.4 (15.7) | all four courses pass: 57.1, 57.3, 56.8, 56.9; 1% low 15.9, 17.3, 16.0, 15.2 |
| Audio | one to three "mixer is finishing its frames late" lines a run, no underrun | the same |
| Looking by touch (`touchControls` on, `MW2_TRACE_LOOK=1`, `adb shell input swipe` on the right half, Combine) | pass: 2 cm turned 26.03 degrees slow (2 s), brisk (0.3 s) and as a flick (0.06 s), either way; 1 cm down and up 12.97; asked 26.04 and 12.98 | pass: 2 cm turned 25.95 of 25.96 at each speed, 1 cm 13.02 of 13.02 |

The Thor's frames waited for the processor 10 to 13% (multiplayer), 25%
(Freerun) and 30 to 79% (zombies) of the time, with the chip at 3.4 to 6.2
ms: that is the 120 work (Bo3Thor120). The Flip holds 60 everywhere, its
chip at 11.4 to 13.7 ms. A `STALL` at a run's end is the After Action
Report, which draws no world. The app's FOV setting now reaches BO3 as
`cg_fov 85` at the start of each run, since the console takes it.

## Toward 120 on the Thor (27 November 2026)

**Where a frame's time went** (The Giant at 120 before, `MW2_REPORT`,
the threads in % of one core, in brackets the share of the time each stood
ready with no core): the ring consumer 75, 3_Server 65 (29), 2_Backend 63
(31), Main 59 (37), the three JQ Workers 46, 45, 30 (43, 43, 58), the
recorder 20 (20); the chip 6.0 to 6.5 ms a frame, at work about 70% of the
time. Every thread the title makes was on the four performance cores
(cpu 3 to 6) with the recorder, and the three efficiency cores were left to
other programs. The title lays its threads out as Advanced Warfare does:
`KeSetAffinityThread` (logged now) puts Main on processor 0, 2_Backend on 3,
3_Server on 5, the JQ Workers on 1, 2 and 4, and Stream, Database, Sound Mix
and Netchan on 4. On Windows (lldb, 50 samples of every thread) Main and
3_Server are 92 to 97% busy at 120 a second: they wait for each other
by polling an event between jobs (sub_826B4448 calling sub_826662B8,
`WaitForSingleObject(h, 0)`), and a quarter of Main's samples stood on the
kernel's dispatcher lock or in the two heap blocks every wait took.

**What changed.**
- `T_FRAME_PROCESSORS` is Advanced Warfare's for Black Ops III: the JQ
  Workers and the streaming threads may run on the efficiency cores
  (`MW2_FRAME_PROCESSORS` measures another layout).
- A poll that finds nothing yields the processor (`T_POLL_YIELD`, on for
  this title; `MW2_POLL_YIELD`), and a wait keeps the objects it names on
  the stack (every title).
- The scene rule (`renderer_scene.cpp`, every title) draws the scene
  smaller only when the chip was at work for 85% of a window's own time or
  more, and measures the room for the step back up against the frames' own
  period where the chip had time to spare. Before, The Giant went down to
  58% while its chip's time a frame went from 6.6 ms at 84% to 6.3 at 58%.
- The `performance:` line gives the slowest hundredth of frames (the
  play's 1% low, `adaptive::Tally`).

**The Thor, The Giant at 120** (one run each, caches warm from the run
before but for the first; mean of the seconds of play after the first ten,
1% low of the play's frames):

| | mean | 1% low | longest | seconds at 115+ | scene, mean (least) |
|---|---|---|---|---|---|
| before (Bo3Fps, first visit) | 107.0 | | 118 ms | 30% | 68% (58) |
| old core layout, new scene rule | 109.7 | 25.8 | 97 ms | 45% | 73% (58) |
| new layout | 112.0 | 34.0 | 81 ms | 66% | 90% (64) |
| new layout and the poll's yield | 114.6 | 59.5 | 56 ms | 80% | 91% (58) |

With both, the threads stood ready with no core 18 to 40% of the time for
20 to 59% before. The scene's step back up against the frames' own period
came after these runs and is not in them. Shadows of Evil with both on its first visit (pipelines
compiled in play): 99.3, 1% low 26.5, the longest 120 ms; its long frames
were the renderer's thread standing up to 32 ms for one pipeline
(`building pipelines (1)`) and the title presenting 30 to 45 ms late.

**The game's timing with the changes** (Windows, the same build, 120
against 60): the server frame 49.99 against 50.02 ms (multiplayer), 50.09
against 50.14 (Shadows of Evil); the KN-44 fires 21 rounds in about two
seconds at both; a reload is back to 30 within one dump (0.26 s) of the
other; a one-second sidestep moves the minimap 11 px at both.

Open: the warm-cache runs of Shadows of Evil and a second multiplayer map,
a ten-minute run for the heat, the Flip at 60, and the other titles'
scenarios on this build (the scene rule and the wait list are theirs too)
were not measured in this round.

## Open

- The zombies once ended in `Could not load default asset '' for asset type
  'rawfile'` (db_registry.cpp:3171) during play, and two copies loading the
  front end at the same moment once both read rubbish (`MT_GetSize: max
  allocation exceeded`, unusable shaders); a single copy idle at `Press A`
  for two minutes once faulted in sub_8235C440 on a table pointer of -1.
  Not reproduced on demand, and not seen in the forty runs of 26 November;
  all look like the title reading memory another thread has not finished
  with.
- A start that hung in the driver, in a process nothing could end: each of
  the five such processes was started within minutes of an nvlddmkm 153,
  and those followed the target dumps (`MW2_DUMP_TARGETS`), which copied
  16_16_16_16_FLOAT surfaces into a buffer of half their size. The dump is
  sized by the format now. A fault inside a library is also left to the
  library on Windows: the driver handles one of its own when it cannot make
  a device, and the runtime used to end the process there.
- A window covered by others presents four frames a second and the game
  slows with it.
- On Windows The Giant and Shadows of Evil were played to round 1 and
  round 2, Der Eisendrache to round 1; on the phones all three to round 1.
  Zetsubou No Shima, Gorod Krovi and Revelations are not on this PC.
- The thin streak from a zombie's head (above) is still there. It can be
  caught on Windows: one 300 s Shadows of Evil run with `MW2_FLASH_LONE=6
  MW2_FLASH_FRAMES=5 MW2_FLASH_DRAWS=1` kept it in frame 6741, 118 s in, a
  yellow line from the nearer zombie's eyes to the left edge, absent in
  6740 (and, 8 frames later, a black wedge over the sky for one frame).
  `tools/flash_draws.py` could not name the draw: the mark fell on 6740
  (the lone frame is the one before the newest), so 6741 was the last frame
  kept, and the tool needs two on each side. Not fixed.
- In `android`, and not released: nothing is published for this title, and
  the release script leaves its paths out of every mirror
  (`tools/release/release.py`, `UNRELEASED`).
