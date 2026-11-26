# Building the game code on the phone

The game code is each player's own: their disc's executables, translated to
C++ by XenonRecomp and compiled for arm64 ([game-library.md](game-library.md)).
Until now that took a PC (`setup\setup.ps1`). With the build tools installed
beside the app, the phone does it: the same recompiler, the same compiler line,
the same link, and a library that states what the PC's states.

First the tools: the package `com.recomp.toolchain`, what is in it, where it
comes from and how it was tried. Then [the app's side](#the-apps-side):
finding the executables, running the build, loading the result.

## The toolchain package

`com.recomp.toolchain` is an app with no code, no screen and no permissions
(`android/toolchain`), as the game code's package is. It holds nothing of any
game, so it is published: one APK, the same for every game, signed with the
release key.

| in the package | what | from |
|---|---|---|
| `lib/arm64-v8a/libllvm.so` | clang and lld 19.1.7 in one program, arm64 code generation only (90 MB unpacked) | LLVM's release sources, cross-built with the NDK (`tools/toolchain/build_llvm.sh`) |
| `lib/arm64-v8a/libXenonRecomp.so`, `libXenonAnalyse.so` | the recompiler and its analyser (1.4 and 1.0 MB) | `XenonRecomp/` at the pinned commit with `patches/xenonrecomp-mw2.patch` (`build_recompiler.sh`) |
| `assets/toolchain/data.zip` | what clang reads: Android's C headers, libc++, the compiler's headers and runtime, what a link needs of libc, libm and libdl, the recompiler's `ppc_context.h` and SIMDe (9 MB; 51 MB and 2,500 files unpacked), and every notice | AOSP bionic and LLVM sources (`build_sysroot.sh`), `XenonRecomp/` |
| `assets/toolchain/toolchain.json` | versions, names and the command lines | `package.py` |

The APK is 43 MB.

**Why the programs are called `lib*.so`.** An app may run a program only from
a package's library folder (an app's own files cannot be executed since
Android 10), and Android unpacks into that folder only files named `lib*.so`.
The folder of another package is readable and executable, so the game's app
asks the package manager where the toolchain's is
(`getApplicationInfo("com.recomp.toolchain").nativeLibraryDir`; its manifest
needs `<queries>` for the name) and starts the programs with `ProcessBuilder`.

**Why clang and lld are one program.** They share most of LLVM; apart they
would be 140 MB. `libllvm.so clang ...` is clang, `libllvm.so ld.lld ...` is
the linker. Clang cannot start the linker itself, because it looks for a file
called `ld.lld`; so the link is run directly, with the arguments the NDK's
clang gives its linker.

**Why the data is a zip in the assets.** Clang needs real files. The game's
app opens the toolchain's APK (`applicationInfo.sourceDir`, readable by any
app), takes `assets/toolchain/data.zip` and unpacks it into its own files,
once for each version (`data.sha256` in `toolchain.json`). No provider, no
shared storage.

### toolchain.json

```json
{
  "schema": 1,
  "version": "1",
  "clang": { "version": "19.1.7", "target": "aarch64-none-linux-android26" },
  "recompiler": { "commit": "ddd128bc...", "patchSha256": "937e906d..." },
  "data": { "asset": "toolchain/data.zip", "sha256": "..." },
  "tools": { "llvm": "libllvm.so", "recomp": "libXenonRecomp.so", "analyse": "libXenonAnalyse.so" },
  "paths": { "simde": "XenonRecomp/thirdparty/simde", "context": "XenonRecomp/XenonUtils/ppc_context.h", "licences": "licences" },
  "compile": ["{llvm}", "clang", "--driver-mode=g++", "--target=...", "--sysroot={data}/sysroot", "..."],
  "linkPrefix": ["{llvm}", "ld.lld", "..."],
  "linkSuffix": ["-latomic", "-lm", "..."]
}
```

`{llvm}` is the program's path, `{data}` the folder `data.zip` was unpacked
into. A compile is `compile`, the build's own flags (`-O3 -DNDEBUG -fPIC -w
-fwrapv -fno-omit-frame-pointer -std=gnu++20`, the title's definitions and
include folders, `-include runtime/mmio_hook.h`), `-c`, the source and `-o`.
A link is `linkPrefix`, the build's own options (`--build-id=sha1
--gc-sections -z common-page-size=16384 --dynamic-list=... 
--unresolved-symbols=ignore-all -soname ... -o ...`), the objects, `linkSuffix`.
`compile` is what the NDK's CMake toolchain file gives every compile of the PC
build, less `-g`; the two link halves are the command the NDK's `clang++`
runs for `-shared -static-libstdc++`. `recompiler` is what the game library's
statement names, so an app can refuse a toolchain whose recompiler is not the
one it was built against before it has compiled anything.

What belongs to a state of the app (the titles' `config/*.toml`,
`runtime/mmio_hook.h`, `cmake/game_library.list`, the statement it expects) is
the app's, not the toolchain's: an update of the app that changes none of the
recompiler needs no new toolchain.

### Where it comes from

Nothing in the package is copied out of the Android NDK; the NDK is only the
compiler the package is built with ([THIRD_PARTY.md](../THIRD_PARTY.md)).

- **clang, lld, libc++, libunwind, the compiler's runtime and headers**: the
  LLVM 19.1.7 release, the line the NDK's own clang (19.0.1) is cut from.
  libc++ is configured as the NDK's is (`std::__ndk1`, static, its ABI library
  apart).
- **Android's C headers**: AOSP bionic at the head of `ndk-r28-release`, laid
  out as bionic's own build lays them out for the NDK. Checked: a recompiled
  file compiled by the NDK's clang against these and against the NDK's own
  sysroot gives the same object, byte for byte.
- **`libc.so`, `libm.so`, `libdl.so` for the link**: empty libraries with the
  names and versions bionic's `*.map.txt` list for API 26
  (`tools/toolchain/ndk_stubs.py`). Checked: the same exported symbols,
  versions and types as the NDK's three, all 1,519.
- **`crtbegin_so.o`, `crtend_so.o`**: compiled from bionic's sources.

### Building it

    NDK=<an NDK r28> tools/toolchain/build.sh <work directory>

on Linux (CI) or in Git's bash on Windows; it needs cmake, ninja, python3, a
host C++ compiler, a JDK 17 and `XenonRecomp/` (`android/fetch_deps.sh`).
About nine minutes on 32 threads, eight of them LLVM. The result is
`out/toolchain/recomp-toolchain.apk`, signed with `MW2_KEYSTORE` when the
environment has it, as the app's release is.

The published package is CI's: `.github/workflows/toolchain.yml` runs the
same script on a GitHub runner (45 minutes on four threads, measured), signs
with the release key and drafts the release the apps download from
([android-releases.md](android-releases.md#the-toolchain-release)).

### Trying it on a device

What an adb shell may run says nothing of what an app may, so there is a test
app (`android/toolchain-probe`, never published) that runs a command as an app
does, with the tools found through the package manager, and loads a library
the command linked. `tools/toolchain/device_test.py <title>` pushes a title's
executable and the few files the build reads, has the probe run
`tools/toolchain/device_build.sh` (the whole build as a shell script:
analyse, recompile, the shared header, the statement, every compile, the
link), and compares the statement of the library with what an app built from
the checkout expects and with the PC build's.

### What was measured

Modern Warfare 2's campaign (`default.xex`, 113 files of C++, 63 MB), built by
the probe app with `-O2`, eight compiles at a time, no debug information:

| | AYN Thor (Snapdragon 8 Gen 2, 16 GB, Android 13) | Retroid Pocket Flip 2 (Snapdragon 865, 8 GB, Android 13) | PC (i9, 32 threads) |
|---|---|---|---|
| analyse and recompile | 12 s | 16 s | about 10 s |
| compile | 70 to 73 s | 131 s | about 50 s (`-O3 -g`) |
| link | under 1 s | 1 s | |
| memory: all compilers together, at most | 1.5 GB | 1.5 GB | |
| memory: the largest one compiler | 250 MB | 250 MB | |
| memory the phone had left, at least | 11.6 GB | 3.9 GB | |
| `libmw2game.so` | 18.5 MB | 18.5 MB | 63 MB with debug information |

- The recompiled tree and the jump tables are the PC's, byte for byte (the
  three files `tools/genshared.py` writes aside, which the build writes itself).
- The library's statement is `mw2 d5de1f8e8711` on both: the PC build's, and
  what an app built from the checkout expects (`config/abi/mw2/`).
- The Thor's library has the PC library's dynamic section and its dynamic
  symbol table, all 55,932 symbols with their versions and bindings.
- Running the programs out of the toolchain package's library folder from
  another app, and `System.load` of a library that app had just linked into
  its own files, both work on both devices as they are sold (Android 13).

Found on the way, for whoever writes a build around the tools:

- A build must be in the foreground. A background app's processes are kept to
  three small cores (`/dev/cpuset/background/cpus` is `0-2` on the Thor).
- Ask for eight compiles by number: `nproc` says 6 in a shell on the Thor.
- The phone's shell is mksh with toybox: `hash` is a command of the shell's
  (a function of that name is never called), `grep` has no `\b`, `xargs -P`
  works.
- The debug information is what makes the PC's compile heavy (a gigabyte a
  compiler). Without it a phone of 8 GB has room for eight compilers.

## The app's side

A player with the app and a disc image needs nothing else. The guided setup
([android.md](android.md#the-guided-setup-setupkt-setupactivitykt)) takes the
image and starts the build with one button, and shows it as one bar. Under
the launcher's **Repair / rebuild**, **Build the game again**
(`BuildActivity.kt`) is the same build with its parts named: it says what
is there and offers the one button for what is missing:

1. **The executables.** `default.xex` and `default_mp.xex` are taken from the
   installed game files, from a disc image of this game found on the device
   or a memory card (`DiscImage.search`: every folder down to four below a
   storage's root, each image told by the title number in its executable and
   not by its name; it finds nothing where the app may not list folders), or
   out of the image the player picks, with the Files app or in the app's
   browser, where the folder the image lies in can be chosen as well
   (`DiscImage.kt` reads the image's folders; nothing else of it is read).
   Each is kept only if its SHA-256 is a build the app supports, so an image
   of another game is passed over. The player is never asked for the
   executables themselves.
2. **The toolchain package.** Found through the package manager
   (`Toolchain.kt`). When it is missing, or is of another version or has a
   recompiler that is not the app's (`toolchain.json`: `version`,
   `recompiler`), the build itself downloads `recomp-toolchain.apk` from the
   release `setup/toolchain.json` names (one release for all the games, the
   list being part of the build kit) into the app's cache, compares the
   file's SHA-256 with the list's, and only then gives it to Android's
   installer as a session (`ToolchainInstall.kt`); a file that is anything
   else is deleted. Android asks the player once whether the app may install
   it (on a device that never let this app install anything, it first asks
   for that permission in its settings). The app is then the package's
   installer, and Android 12 and later let an installer replace its own
   package by a newer one without asking: later toolchains arrive with no
   question at all.
3. **The build** (`GameBuild.kt`), in a foreground service
   (`GameBuildService.kt`) with a wake lock and a notification: unpack the
   toolchain's data (once), then for each title analyse, recompile, write the
   shared header, compare the statement, compile what is not compiled
   already, and link.

### What travels with the app: the build kit

Everything that decides whether the app's runtime accepts the result is in
the app, as assets under `buildkit/` that Gradle makes from the checkout at
build time (`android/app/build.gradle.kts`, `gameBuildKit<Game>`): the titles'
`config/*.toml` as they are, `runtime/mmio_hook.h`, `cmake/game_library.list`,
the text `tools/genshared.py` writes before its declarations, and `kit.json`
with the executables' hashes, the compiler's and linker's options as
`CMakeLists.txt` and `cmake/game_library.cmake` give them (`-O3`, less `-g`),
and for each library the statement the runtime expects, composed as
`cmake/game_abi.cmake` composes it from `config/abi/`, with the earlier
statements it still takes ([game-library.md](game-library.md#what-they-agree-on)).

The work folder in the app's private storage is laid out as the repository
is, so the configuration files are used unchanged. `tools/genshared.py` is
done by `GameBuild.writeSharedHeader`, `tools/intext_switch_tables.py` (Call
of Duty 4) by `InTextTables.kt`. Before the first compile the statement of
what was actually made (the recompiled tree's SHA-256, the code's extent,
the register context, the system calls) is compared with the kit's; a
difference stops the build in half a minute and names the line. The tree a
phone's recompiler writes is the PC's byte for byte, which is what lets one
SHA-256 stand for both.

### Loading: a file in the app's own storage

The library is left in `files/game-code/lib<title>game.so` with the statement
beside it, and `GameLibrary.kt` loads it from there, before the game package
of a PC build (which is still used when the first is refused). An app may not
*run* a program it wrote, but it may *load* a library it wrote: on the AYN
Thor and the Retroid Pocket Flip 2 (Android 13, targetSdk 35) `dlopen` of the
file works with no SELinux denial, so no package is assembled and signed on
the device and the game package's rules are unchanged. The runtime checks
the statement exactly as it does for a package.

### The multiplayer a title update makes

Advanced Warfare's map packs are for the multiplayer as the game's title
update makes it, which is another executable and so another library,
`awmp17` ([dlc.md](dlc.md#advanced-warfare-map-packs-and-exo-zombies)). The
kit lists it as a third title, marked as made from an update
(`fromUpdate`, with a folder of its own, `aw/tu17`), and the build takes it
up only while a title update's package is in the app's `dlc` folder
(`GameBuild.needed`): an app without the player's update builds two
libraries as before.

- **Its executable** is not on the disc. The runtime's own patcher, the one
  that makes it in memory at every start of the updated game, writes it once
  (`NativeBridge.nativeMakeUpdatedExecutable`,
  `install::MakeUpdatedExecutable`): the disc's `default_mp.xex`, kept from
  the first build, with the `default_mp.xexp` of the update's package
  applied, and kept only when its image is title update 17's by its
  SHA-256. An update of another version says so there and nothing is
  compiled. A note beside the executable names the package it came from.
- **Its jump tables.** Besides the kind Call of Duty 4 has, this executable
  has one table of halfwords with no bound before it, which the analyser
  passes over; `InTextTables.kt` adds it as `tools/intext_switch_tables.py`
  does, from the function's extent in `.pdata`.
- **When.** Add DLC starts the build when it has copied an update's package
  whose game code is not here (`DlcActivity`), as the player's own start:
  the toolchain package may be downloaded for it. A build already under way
  takes the new title up when it has finished the others (`GameBuild.run`
  looks again at what is wanted). After an update of the app that expects
  other code, the third library is rebuilt with the other two.
- **Where it is used.** `GameLibrary` knows a library by its name, so the one
  built here is found for the updated multiplayer as the others are, before
  a game package's from `setup -Dlc`.

**Measured on the Retroid Pocket Flip 2** (25 November 2026, a newly
installed app, no game package, the disc images and the `41560914` folder on
the memory card; nothing came from a PC but those files and, so as to leave
the device's own toolchain package alone, a toolchain package under another
name installed beforehand). The first build, campaign and multiplayer, took
13 minutes with the app replaced once on the way. Add DLC then copied eleven
packages (3.5 GB: the title updates, the six Compatibility Packs, Havoc,
Atlas Gorge, the Season Pass) in under a minute and started the third build
by itself: the executable 0.8 s, analyse and recompile 34.5 s (the statement
check passed, so the tree is the PC's), 212 files compiled in 322 s with
eight compilers, the link 0.3 s; **5 minutes 58 seconds** in all, 3.8 GB
free at least, battery 34.7 °C, no thermal status. The Add DLC screen then
read "Multiplayer now starts as the updated game", the launcher had **Exo
Zombies**, and Multiplayer came up as version 1.20 with its Store entry at
60 frames a second from `libawmp17game.so`. A match on a pack's map was
**not** played in this run. Evidence: `private-captures\aw\flip-dlc-ondevice-*`.

### Pausing, interruptions, memory and heat

- Every step leaves its result under its final name only when it is whole
  (an object is `*.o.part` until the compiler has ended well), and the build
  looks at the files and does what is left. Pause ends the compilers and
  keeps the objects; a build whose process the system ended is started again
  by the sticky service or, failing that, when the launcher is next opened.
  Seen on the Thor: the app replaced in the middle of a build, and a pause at
  36 of 226 files, both carried on from where they were.
- As many compilers as cores, fewer when memory is short (400 MB counted for
  each, 1 GB left to the system; a compiler the system kills is run again
  with one fewer), half when Android reports a severe thermal status or the
  battery passes 46 °C, one when critical or past 50 °C.
- The screen shows the step, files done of the total, the time left, the
  number of compilers and the battery's temperature, and asks for the charger
  when it is not connected. `game-build-report.txt` in the app's folder on
  shared storage has each step's time, for a bug report.

### After an update of the app

An update that expects other game code than the device has
([android-releases.md](android-releases.md#when-the-game-code-has-to-be-rebuilt))
needs nothing from the player (`GameUpdate.kt`):

- **It starts by itself.** Android tells the app that it was replaced
  (`MY_PACKAGE_REPLACED`); the app reads what each library on the device
  states (out of the package's file, without loading it) and, when this
  version takes none of a title's, starts the build in the same foreground
  service, with the notification **Updating the game code**. The same is
  looked at whenever the launcher is opened, for whatever kept it from
  starting then. The executables are the ones kept from the last build, or
  are read again from the installed game files, the disc image chosen
  before, or an image found on the device: no image is asked for.
- **Only what changed is compiled.** The objects of the last build stay in
  `files/game-build/obj/<library>/`, each with a note of what it was made
  from: the SHA-256 of its source, of `ppc_config.h`, `ppc_context.h` and
  `runtime/mmio_hook.h`, the compiler's command line and the toolchain's
  compiler and data. The executables are recompiled (the tree is deleted
  after every build, being the larger part), and a file whose note still
  holds is not compiled again. The shared header lists every function of
  the tree, so a function more or fewer would be every file's change; only
  what the header says besides that list is in the note, since a file
  compiles the same whichever other functions are declared.
- **The battery.** A build the app started itself runs on the charger, or
  with the battery at or above a charge the player sets (**Settings →
  Update the game code by itself**: never, only on the charger, above 80,
  50, 30 or 15%, or always; 30% unless changed). Below it the build stops
  and starts again when the charger is connected (a job the system runs
  then) or the app is opened. Memory and heat hold it back as they do any
  build.
- **What it will not do by itself:** download the toolchain over a metered
  network, or install a toolchain on a device that never had one. It then
  says so in a notification that opens the build screen, and tries again
  after the next update of the app or when the player starts the build. A
  build that failed is not started again by itself either.
- **Meanwhile** the launcher's status says that the code is being updated
  and how far it is, and Play waits for it; a title whose code did not
  change is played as before.

Tried on the AYN Thor (Android 13, on the charger) with Modern Warfare 2,
apps under a name of their own beside the published one, the toolchain
served as [android-releases.md](android-releases.md#trying-the-update-without-publishing)
says, November 2026; the times are `game-build-report.txt`'s:

| An update of the app over | What happened | Time |
|---|---|---|
| the release before, with game code built on a PC before `toolchain-v2` (a statement of the old kind, the same code) | nothing built: the game was at its title screen a second after the update was installed and the app opened. The app before the update refused that library (`its recompiler-patch is not the one this app was built for`) | 0 s |
| the same app, with a configuration that makes one function two (10 of the campaign's 111 files differ, the shared header among the tree's) | built by itself in the background, with the launcher not opened: recompiled 13.1 s, compiled 10 files 8.5 s, kept 101 objects | 21.7 s |
| the same, and a toolchain package of a newer version | the same, after downloading the package and installing it over the old one, with no question from Android (0.9 s) | 22.6 s |
| no objects kept (the first build, both titles, 226 files; Android asked once about the toolchain package) | recompiled 26.5 s, compiled 167 s | 194 s |

A library that is other code than its note says (the campaign's swapped for
one made from the other configuration, the note left) is passed by the
launcher and refused by the runtime when the game starts, `its
generated-code is not the one this app was built for`; the message's
**Build on this device** then made it again from the objects kept in 13 s,
all of it the recompiler's.

### What was measured (the whole flow, in the app)

Both titles of a game, `-O3`, eight compilers, from the Build button to both
libraries in place:

| | AYN Thor, Modern Warfare 2 | AYN Thor, Modern Warfare 3 | Retroid Pocket Flip 2 (8 GB), Modern Warfare 2 |
|---|---|---|---|
| analyse, recompile, header (campaign + multiplayer) | 13 s + 14 s | 16 s + 18 s | 17 s + 18 s |
| compile | 74 s + 80 s (226 files) | 115 s + 129 s (295 files) | 142 s + 149 s |
| link | 0.1 s each | 0.1 s each | 0.1 s each |
| the whole build | about 3 min | 4 min 40 s | 5 min 30 s |
| all compilers together, at most | 1.4 GB | 1.5 GB | 1.4 GB |
| free memory, at least | 10.6 GB | 10.4 GB | 3.2 GB |
| battery | 28 °C | 28 °C | 33 to 34 °C |
| hottest CPU sensor / board sensor | 93 °C / 57 °C | 92 °C / 58 °C | 90 °C / 59 °C (skin) |
| Android's thermal status | none | none | none |

The other builds, whole, from the app's own report:

| | AYN Thor | Retroid Pocket Flip 2 (8 GB) |
|---|---|---|
| Call of Duty 4 (151 files) | 2 min 12 s (recompile 11 s + 11 s, compile 54 s + 57 s) | 3 min 49 s (14 s + 14 s, 99 s + 101 s) |
| Modern Warfare 3 | 4 min 40 s | 8 min 2 s (21 s + 23 s, 211 s + 226 s), a few seconds of it with fewer compilers |
| Advanced Warfare (409 files) | **not run** | 13 min 8 s with the published v0.1.0 app and toolchain-v1 ([AW-BRINGUP.md](AW-BRINGUP.md#the-published-app-as-a-new-player-24-november-2026)); in the earlier run 12 min 41 s (31 s + 37 s, 339 s + 352 s); eight compilers throughout, 1.8 GB between them at most, 3.3 GB free at least (the report; 3.7 GB by `MemAvailable`), battery 30 to 31.5 °C, hottest sensor 91 °C, no thermal status |

**Played on what was built**, with no game package installed:

- Call of Duty 4, whose jump tables `InTextTables.kt` adds: the statement
  check passed on both devices, F.N.G. loads and is walked at 119 frames a
  second on the Thor and 60 on the Flip 2, and the Thor hosted a System Link
  match on Backlot from `libcod4mpgame.so`.
- Advanced Warfare, whose jump tables `InTextTables.kt` adds too: the
  statement check passed on the Flip 2, and with its game package
  uninstalled the campaign resumed Atlas's funeral from `libawgame.so` at
  60 frames a second. The executables were the installed game files'; the
  disc image lay in Download and was not needed. The multiplayer's library
  was built and **not run**.
- Modern Warfare 2: the main menu and a System Link match on Afghan (Thor);
  S.S.D.D. on the Flip 2 from the image on its memory card.
- Modern Warfare 3: its opening film (Thor), a System Link match on Seatown
  (Flip 2).

**The toolchain from its release.** With no toolchain installed, the app
downloaded the release's own APK (CI's, signed with the release key) from a
server standing in for GitHub (`MW2_TOOLCHAIN_URL`), Android asked to allow
the app to install packages and then to install it, and the build went on
from there, on both devices. An APK with another hash, served under the
same name, was deleted and said to have "not arrived whole".

**Memory and heat, made to happen.** On the Flip 2 during a build:

- Another program taking 1.7 GB left 1.3 GB free: the screen said
  "fewer: memory is short", seven compilers ran, then eight again.
- The same taking 4.4 GB left 0.4 GB: Android ended the app's process with
  its compilers; the service started again by itself and the build carried
  on from the files it had, to the end. The report then has only the part
  after the restart.
- Modern Warfare 3's own app started beside it and played its opening film
  at 60 frames a second while eight compilers ran, 3.1 GB free.
- The battery's temperature reported as 47 °C (`dumpsys battery set temp`):
  four compilers, "fewer: the device is hot"; as 51 °C on the Thor: one;
  back to eight when it was put back.

**The disc image, chosen once.** With no image in Download, the build screen's
**Choose the disc image with the Files app** took one from the Flip 2's
memory card (`ROMs/xbox360/`), with no storage permission asked; the app
keeps the permission to that document, and the game was then played from
the same image without asking again.

Not tried: Android versions other than 13, and devices other than these two.
