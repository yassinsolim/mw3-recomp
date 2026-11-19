# Mods (Modern Warfare 3)

A mods folder for both Modern Warfare 3 titles: game scripts (`.gsc`) and raw
files of your own, laid over the ones in the fastfiles. The idea is CoD Xe's
(a plugin for the console); the code is this port's (`runtime/mods.cpp`), and
the script compiler is [gsc-tool](https://github.com/xensik/gsc-tool) by
xensik (GPL-3.0, `runtime/mods_gsc.cpp`, [THIRD_PARTY.md](../THIRD_PARTY.md)).

**Status: experimental.** See [What is verified](#what-is-verified-and-what-is-not).

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
  source (decompiled; local variables are `var_0`...). Start a match once and
  you have the scripts to read, or to copy into a mod and change. They are
  the game's: do not publish them.
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
  an offset into it), and the pool's free room is lent to the streamed
  textures, so the code's room is asked for through the title's own
  allocator (`PMem_Alloc`).
- Own scripts are loaded while the title's loader is open and their `main`
  and `init` run through `Scr_ExecThread`, as the title runs its own.
- Addresses and the evidence for each: `runtime/title.h`, "Mods".

## What is verified, and what is not

On the PC build of the multiplayer (Team Deathmatch, System Link, host):

- an own script compiled, loaded and ran: `iPrintLnBold` text and a HUD line
  on screen, custom fields on `level` and the player, `#include` of a stock
  script, calls into stock scripts, `main()` and `init()`;
- `MW2_SCRIPT_DUMP` wrote 271 stock scripts; `MW2_TRACE_MODS` listed the raw
  files asked for;
- a replaced stock script (`maps/mp/gametypes/war`, from its dump) compiled
  and was loaded in place of the game's.

Not verified, or known wrong:

- **Stability.** Of the runs that took the code's room through `PMem_Alloc`,
  one that asked for exactly the script's size played to its end; two that
  asked for 64 KB crashed in the title's audio code a few seconds into the
  match. The cause is not known (the suspicion: the port's delayed copy of a
  moved texture, `runtime/image_move.cpp`, landing after the code was
  placed). The committed code asks for the exact size; it has not been run
  enough to call it stable, and not at all with a large mod.
- The first request for room waits for the renderer, which added about 3 s
  to a level load.
- A replaced stock script was not seen running to the end of a match, and a
  replaced raw file was not tried.
- The campaign: its hooks were found by the same code patterns and are
  compiled in, but that title was neither built nor run with them.
- Android: not built, not run. The app's MODS entry only explains the folder.
- The launcher's script-mod install: written, not compiled or run.
- Map entities (`.ents`) are not replaced.
- A campaign save holds script state; loading one made with other mods on is
  untested.
