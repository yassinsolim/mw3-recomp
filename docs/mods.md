# Mods (Modern Warfare 3)

A mods folder for both Modern Warfare 3 titles: game scripts (`.gsc`) and raw
files of your own, laid over the ones in the fastfiles. The idea is CoD Xe's
(a plugin for the console); the code is this port's (`runtime/mods.cpp`), and
the script compiler is [gsc-tool](https://github.com/xensik/gsc-tool) by
xensik (GPL-3.0, `runtime/mods_gsc.cpp`, [THIRD_PARTY.md](../THIRD_PARTY.md)).

**Status: experimental.** See [What is verified](#what-is-verified-and-what-is-not).
On Android the `hello` example was run in a multiplayer match on an AYN Thor.

## Installing a mod

    <game>/mods/mods.txt        the mods that are on, one folder name a line
    <game>/mods/hello/...       a mod

- A mod is on when `mods.txt` names its folder. Lines starting with `#` are
  comments. When two mods have the same file, the one listed first wins.
  Nothing is read from a folder `mods.txt` does not name.
- The desktop launcher's MODS entry does this for you: choose a mod's folder
  and it is copied to `<game>/mods/` and added to `mods.txt`; choose its
  folder inside `<game>/mods/` again to turn it off (and again, on).
- On Android, copy the folder into the game folder's `mods/` with a file
  manager and edit `mods.txt`.
- `MW2_MOD_DIR=<folder>` names a mods folder elsewhere ([switches.md](switches.md)).
- Scripts run on the host. In a match somebody else hosts, your mods do nothing.

`mods-examples/hello/` is a complete small mod for the multiplayer.

## Writing a mod

A file in a mod stands for the game's asset of the same path.

| file | what it does |
|---|---|
| `maps/mp/gametypes/war.gsc` | replaces that stock script (any script path works) |
| `scripts/x.gsc`, `scripts/mp/**.gsc` (multiplayer), `scripts/sp/**.gsc` (campaign) | a script of your own that starts by itself (below) |
| any other `.gsc`, e.g. `scripts/lib/util.gsc` | a script of your own, reached from other scripts: `scripts\lib\util::f()` or `#include` |
| `x.gscbin` | the same, already compiled with gsc-tool (`-g iw5 -s xb`); the `.gsc` wins when both exist |
| any other path, e.g. `mp/basemaps.arena` | replaces that raw file |

Scripts that start by themselves:

- Multiplayer: `main()` runs after the game type's own `main()` and before
  the map's; `init()` runs once the game type has started.
- Campaign: `main()` runs before the level's `main()`; `init()` after the
  level's has run to its first wait.

The folders are read again at every level load, so an edited script runs from
the next match. A script that does not compile is reported in the log
(`mods: ... does not compile: <file>:<line>: ...`) and the game's own is used.
A script's path is at most 59 characters.

`#include` and `path::function()` reach the game's own scripts as well as the
mod's.

### Seeing the game's own scripts and files

- `MW2_SCRIPT_DUMP=<folder>` writes every stock script a level loads there as
  source (decompiled; local variables are `var_0`...), and every raw file
  the game reads as it is. Start a match once and you have the files to
  read, or to copy into a mod and change. They are the game's: do not
  publish them.
- `MW2_TRACE_MODS=1` logs each script and raw file the game asks for, once:
  the paths a mod can replace.

### Built-ins

The built-in functions and methods are this build's own; none are added.
gsc-tool's lists name them (`src/gsc/engine/iw5_xb_func.cpp`, `..._meth.cpp`:
455 functions, 779 methods across both titles; each title has its own share).
In the multiplayer these exist but do nothing (development-only on the
console): `print3d`, `line`, `createprintchannel`, `setprintchannel`,
`openfile`, `closefile`, `fprintln`, `fprintfields`, `freadln`, `fgetarg`,
`logstring`, `logprint`, `addtestclient`, `setarchive`, `matchend`,
`endparty`; methods `noclip`, `ufo`, `setreverb`, `deactivatereverb`,
`setviewangleresistance`, `setturretaccuracy`, `setmotionblur*scale`,
`updateplayermodelwithweapons`. So Bot Warfare's file and bot built-ins are
not there; bots are the port's own ([switches.md](switches.md), `MW2_BOTS`).

## How it works

- The executable has no script compiler. Its loader (multiplayer
  `sub_822BA050`) asks the asset database for a `scriptfile` (type 0x25:
  name, compressedLen, len, bytecodeLen, buffer, bytecode) and a raw file is
  type 0x24. `DB_FindXAssetHeader` and `DB_IsXAssetDefault` are hooked; for a
  path a mod has, the answer is an asset made by the runtime. A stock script
  is asked for by its number (`308`), which gsc-tool's table turns into its
  path.
- A `.gsc` is compiled by gsc-tool's IW5 Xbox 360 target. Includes of stock
  scripts are read from the fastfiles' compiled copies in memory.
- The title runs code only out of its 2 MB script pool (a function handle is
  an offset into it, a call into another script a 22-bit distance), and the
  pool's unused room is lent to the streamed textures. Taking room from it
  during a level load went wrong in this port (code written there unasked
  was under a texture seconds later; asked for through the title's
  allocator at that point, matches crashed in the title's audio code). So
  the mods' room is taken once, with the title's own `PMem_Alloc`, inside
  one of the title's own brackets. In the multiplayer that is the start-up's
  `$init`: before any fastfile is loaded or anything streamed, below every
  zone's scripts, and never given back. In the campaign it is the bracket of
  `common.ff`, right above that zone's scripts: a saved game holds every
  script thread's position as its distance from the pool's start, so room
  below the game's own scripts moved them all, and Resume Game on a save
  made without a mod never left the black screen after the briefing when a
  mod was on (v0.1.2 and before; an AYN Thor, the multiplayer-only `hello`
  on). The level's own scripts come down from the pool's high end and are
  not moved either way.
- Own scripts are loaded while the title's loader is open and their `main`
  and `init` run through `Scr_ExecThread`, as the title runs its own.
- Addresses and the evidence for each: `runtime/title.h`, "Mods".

## What is verified, and what is not

On the PC build of the multiplayer (Team Deathmatch on two disc maps, System
Link, host, 3 bots):

- the shipped `mods-examples/hello`: three runs of 150 s each played to
  their end with the HUD line on screen through kills, deaths and respawns,
  and nothing in the log;
- an own script's `main()` and `init()`, custom fields on `level` and the
  player, `#include` of a stock script and calls into stock scripts;
- a replaced stock script (`maps/mp/gametypes/war`, from its dump, with a
  line added) compiled, was loaded in place of the game's and the match
  played;
- a replaced raw file (`maps/mp/gametypes/war.txt` holding
  `dm.txt`'s three lines): the loading screen read "Free-for-all";
- `MW2_SCRIPT_DUMP` wrote 271 stock scripts and the raw files asked for;
  `MW2_TRACE_MODS` listed them.

Not verified, or limits:

- The mods' scripts share 384 KB of compiled code, set aside when the game
  starts and only when a mod is on then: turn a mod on, then start the game.
  It comes out of the 2 MB all scripts share (a Team Deathmatch on a disc
  map leaves 1260 KB unused); a map or mode with far more script than those
  was not tried with mods on.
- A large mod was not tried, nor a second match in one run, nor another
  game type.
- The campaign: its hooks were found by the same code patterns; a replaced
  stock script and a replaced raw file were not tried there.
- Android: `mods-examples/hello` ran in multiplayer matches on an AYN Thor, in
  the app and in the multiplayer test copy; nothing larger was tried there.
  The app's MODS entry only explains the folder.
- The launcher's script-mod install compiles; it was not run.
- Map entities (`.ents`) are not replaced.
- A campaign save holds script state. The game's own scripts lie where they
  do with no mod, so a save made with no mod resumes with one on (Black
  Tuesday on an AYN Thor); a save made while a mod's own threads ran needs
  the same mod at the same size, and loading one without it is untested.
- The campaign on Android: an own script in `scripts/sp/` ran its `main()`
  and `init()` in Black Tuesday started from Mission Select (a line printed
  every three seconds); nothing larger was tried.
