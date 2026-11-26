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

Open on Android:

- **The build on the device is written and not run.** The app's kit has
  both titles as made from the update (`build.gradle.kts`, `GameBuild.kt`),
  the runtime writes the two updated executables for the recompiler
  (`MakeUpdatedExecutable`), and `InTextTables.kt` has the search for the
  unbounded dispatches. None of it has been through a device: the tables
  must come out as the script's to the byte, or the library's statement is
  refused.
- The app's installer does not copy this disc (its files are not `.ff`,
  `.pak` and `.bik` alone): the files were pushed over adb. Add DLC is
  offered and was not used; its wording is Advanced Warfare's.
- Setup on a PC (`setup.ps1 -Game bo3`) does not exist.
- The Retroid Pocket Flip 2 was not run.
- On the Thor's second start the title stopped once with `Exceeded limit of
  80 'physpreset' assets` (db_registry.cpp:1407), started itself again and
  played.

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
- The Giant and Shadows of Evil were played to round 1 and round 2; Der
  Eisendrache was not started.
- Not merged into `android`: the other four games have not been compared
  with this branch (the shader translator and the installer changed).
