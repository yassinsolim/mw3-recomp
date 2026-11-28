# The game code as a package of its own (Android)

The Android app is published; the game is not. The recompiled code is made
from the disc's executables and belongs to whoever owns the disc, so it can be
in no release. Until this split the app's libraries held both, and the app
could only ever be built by each player. Now there are two packages, for each
game:

| package | what is in it | who builds it |
|---|---|---|
| `com.mw3.recomp`, `com.mw2.recomp` | the app and the runtime: `libmw3.so`, `libmw3mp.so` (`libmw2.so`, `libmw2mp.so`), with none of the game's code | the release workflow ([android-releases.md](android-releases.md)); it updates itself |
| `com.mw3.recomp.game`, `com.mw2.recomp.game` | the recompiled code: `libmw3game.so`, `libmw3mpgame.so` (`libmw2game.so`, `libmw2mpgame.so`); no code of its own, no screen, no permissions | the player, from their disc: `setup\setup.ps1` on Windows, `setup/setup.sh` on Linux, or GitHub in a private repository of their own ([BUILD-WITHOUT-PC.md](BUILD-WITHOUT-PC.md)); signed with a debug key of that PC's or that repository's |

Modern Warfare 2's apps of one title each (the `campaign` and `multiplayer`
flavours, which is how its title update 6 is built), the multiplayer test
copy (`-Instance`) and the desktop game are still one library or executable
each (`MW2_SPLIT=OFF`).

A package holds its own game's two libraries and no others. Setup collects
them in a folder for each game (`out\game-libraries\mw2`, `...\mw3`) and
`android/game` packages all of the folder it is given, refusing one with any
other file in it. With one folder for both games, as it was, a PC that had
built both put all four libraries in each package: a Modern Warfare 2
package of 98 MB where its own two are 40 (Modern Warfare 3's are 54).

A player without a PC builds the same libraries on the device itself
([ondevice-build.md](ondevice-build.md)): they are then files in the app's
own storage, which `GameLibrary.kt` loads before the package's, under the
same rules.

## How the two halves find each other

`-DMW2_SPLIT=runtime` builds the app's library, `-DMW2_SPLIT=game` the game's
(`cmake/game_abi.cmake`, `cmake/game_library.cmake`).

- **The app loads the game library by path.** `GameLibrary.kt` asks the
  package manager for the game package's library folder (the manifest's
  `<queries>` names the package) and the runtime `dlopen`s
  `lib<title>game.so` from it (`runtime/android/game_library.cpp`). An app's
  linker namespace permits paths under `/data`, and another package's
  libraries are readable and executable; on the AYN Thor and the Retroid
  Pocket Flip 2 (Android 13) it loads with no SELinux denial logged.
- **The game calls the runtime through the dynamic linker.** The system calls
  the executable imports, and every `sub_XXXXXXXX`, are left to the linker
  (`cmake/game_library.list`); the runtime's library is linked `-z global`,
  which puts it in the group the linker searches before a library's own
  symbols. So a call to a hooked function, and the function table's entry for
  it, bind to the runtime's hook, exactly as the static link chose the strong
  definition over the recompiler's weak one. Everything else in the game
  library (`__imp__sub_X`, the register helpers) is bound inside it.
- **The runtime calls the game through pointers.** Its library has to load
  without the game's, so it names none of its symbols: `GUEST_FUNC` reads the
  function table, and `GUEST_ORIG` (the original of a hooked function) is a
  symbol looked up once when the game library is loaded (`runtime/title.h`).

Every function is replaceable, not a fixed list, so an update of the app that
adds a hook needs no new game library.

What it costs: a call between recompiled functions goes through the linker's
table. Measured on the Thor in Survival (Resistance): 2.3% of the game
library's samples are in that table, 1.2% of the process's CPU time
(simpleperf, 15 s); CPU time a second and the frame rate are the same as the
one-library build's within the noise of two runs each.

## What they agree on

Each library carries a statement of what it is, a line for each thing: the
title, the code the recompiler made (`generated-code`: the SHA-256 of the
recompiled tree, a line for each file that is compiled or included with the
file's own SHA-256 and its name), the code's extent, the register context,
the system calls imported and `runtime/mmio_hook.h`. The runtime compares
the game library's (`mw2_game_abi`) with its own before using anything else
of it, and refuses one that differs, naming the first line that does. A
library that asks for a symbol the runtime has not got fails in `dlopen` and
is reported the same way.

What the code was made *with* is not in the statement: the recompiler, this
repository's patch to it, the title's `config/*.toml`, the jump tables and
the script that writes the shared header. A change of those that leaves the
code as it was leaves the statement as it was, and nobody builds anything
again. They were the statement until November 2026 and stopped every game
whenever one of them changed, Modern Warfare 2's and Call of Duty 4's
included when the recompiler's patch changed and their code came out the
same. A library built then says them and not its code, so
`config/abi/<library>/earlier_statements.list` keeps each such statement
behind the code it is known to have been made from, and the app takes the
ones made from the code it takes now: nobody had to build again to move to
the new statement.

The app knows before a run whether it takes the code on the device
(`GameLibrary.kt`: the statement is read out of the library's file, or from
the note beside a library built on the device). When it does not, an app
that can build the code builds it by itself, in the background, as soon as
the update is installed ([ondevice-build.md](ondevice-build.md#after-an-update-of-the-app));
for a PC it names `setup\setup.cmd -GameOnly` (`-Game mw2 -GameOnly`).

The app's side is built without the disc, from what `config/abi/<library>/`
records of the recompiled tree: `ppc_config.h` (four numbers), the names of
the system calls, and the tree's SHA-256 (`generated_code.sha256`). After a
change of the recompiler, its patch or a title's configuration that changes
the code, recompile and record it:

    cmake ... -DMW2_SPLIT=game -DMW2_ABI_RECORD=ON

and commit `config/abi/` with the change; a configure without it warns when
the tree is not the code recorded. Recording other code deletes the
library's `earlier_statements.list`, which is of the code before. The
recompiled tree notes what it was made with (`recompiled_with.txt`, written
by setup), and setup recompiles a tree whose note is not what the checkout
recompiles with; the files that come out as they were keep their dates, so
only the others are compiled again.

## Limits

- The published Modern Warfare 3 app runs the USA/Europe disc build
  (`MW3_XEX_SHA256` in the release workflow) and accepts that one: a disc of
  the bring-up's other packaging (executable version 22) is refused with its
  version and the one supported. An app built on a PC that has that
  executable in `mw3/tu0` installs and accepts it (`-Dev`). Modern
  Warfare 2 has the one disc build (1.0.557).
- The game package is trusted by its name: any package called
  `com.mw3.recomp.game` (`com.mw2.recomp.game`) whose library states the
  right build is loaded. It is installed by the player, like the app. Its
  signature is not looked at: whoever built it (a PC, or the builder
  workflow in the player's private repository, each with a debug key of its
  own) signed it with a key the app cannot know, and what protects the game
  is the statement, which is checked. Android itself only asks that a
  package replacing another is signed with the same key, so a package from
  another builder goes on after the old one is uninstalled.
- An update that expects other code stops the game until the game code is
  rebuilt: by the app itself on the device, on a PC, or by running the
  builder workflow again. Releases publish the statements the app takes
  (`mw3-recomp-<tag>-game-abi.txt`, `mw2-recomp-<tag>-game-abi.txt`: a line
  for each, the earlier ones among them) and setup warns when what it built
  is none of them. Such changes are made together, in one release
  ([android-releases.md](android-releases.md#when-the-game-code-has-to-be-rebuilt)).
