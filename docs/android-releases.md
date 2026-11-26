# Releasing the Android apps

The app is the runtime without the game's code ([game-library.md](game-library.md)),
so it can be built on a runner that has no game file and published. Installed
copies look at the public repository's releases when they start and from
**Check for updates**, show the release's notes, download the APK and hand it
to Android's installer, which puts it over the installed app. The game
package, the game files and the saves are not touched.

Each game has a public repository of its own, with this same code in it, and
a workflow that runs only there:

| game | repository | workflow | app | release files |
|---|---|---|---|---|
| Modern Warfare 3 | `yassinsolim/mw3-recomp` | `android-release.yml` | `com.mw3.recomp` (`assembleMw3Tu0Release`) | `mw3-recomp-<tag>.apk`, `mw3-recomp-<tag>-game-abi.txt` |
| Modern Warfare 2 | `yassinsolim/mw2-recomp` | `android-release-mw2.yml` | `com.mw2.recomp` (`assembleMw2Tu0Release`) | `mw2-recomp-<tag>.apk`, `mw2-recomp-<tag>-game-abi.txt` |
| Call of Duty 4: Modern Warfare | `yassinsolim/cod4-recomp` | `android-release-cod4.yml` | `com.cod4.recomp` (`assembleCod4Tu0Release`) | `cod4-recomp-<tag>.apk`, `cod4-recomp-<tag>-game-abi.txt` |
| Call of Duty: Advanced Warfare | `yassinsolim/aw-recomp` | `android-release-aw.yml` | `com.aw.recomp` (`assembleAwTu0Release`) | `aw-recomp-<tag>.apk`, `aw-recomp-<tag>-game-abi.txt` |

## Once: the key and the secrets

Every release has to be signed with the same key: Android refuses an update
signed with another, and a lost key means every player uninstalling.

- The keystore is **outside the repository**, never committed:
  `C:\Users\solim\mw3-android-work\keys\mw3-release.jks` (alias
  `mw3-release`), its passwords in `mw3-release.properties` beside it and its
  base64 in `mw3-release.jks.base64`. Keep a copy somewhere else as well.
  Its certificate's SHA-256 is in the workflow
  (`RELEASE_CERTIFICATE_SHA256`), which fails on any other.
- In each public repository (Settings, Secrets and variables, Actions) set
  `MW3_KEYSTORE_BASE64` (the contents of the `.base64` file),
  `MW3_KEYSTORE_PASSWORD`, `MW3_KEY_ALIAS` (`mw3-release`) and
  `MW3_KEY_PASSWORD`. The other three repositories use the same key under
  the same names.

## Each release

1. The public repository's default branch has the commit to release, and
   `tools/visualqa/visualqa.py --suite` has passed on it on both phones
   ([visualqa.md](visualqa.md)): every scenario recorded, every frame
   checked for flicker, blocks, missing ground, black frames and stalls, on
   the bundled Turnip driver. A failure is fixed or explained in the notes
   before the tag; a release is not drafted over one.
2. Push a tag `vMAJOR.MINOR.PATCH` there (minor and patch below 100). The
   version code is `MAJOR*10000 + MINOR*100 + PATCH`, and has to be above the
   last release's and above 16, what the apps built by setup carried.
3. The game's workflow builds the app with `-DMW2_SPLIT=runtime`, runs
   `tools/check_runtime_apk.py` on it (no recompiled code, no game files),
   checks the signature and drafts a release with the APK and the statement
   its game code has to match.
4. Read the draft, write the notes (the app shows the first 1500 characters
   as the changelog; say so when the game code has to be rebuilt) and
   publish. Drafts are invisible to the apps.

## When the game code has to be rebuilt

An app takes only game code whose statement is its own
([game-library.md](game-library.md#what-they-agree-on)), and the statement
names the code itself, the SHA-256 of the recompiled tree. So a release
stops the players' games only when it expects *other code*: a change of the
recompiler, its patch or a title's configuration that makes the recompiler
write something else, or a change of `tools/genshared.py`'s header or
`runtime/mmio_hook.h`. A change of the tools that leaves a title's code as
it was costs that title's players nothing. Which it is shows when the tree
is recorded (`-DMW2_ABI_RECORD=ON`): `config/abi/<library>/generated_code.sha256`
changes or does not. Changes that do are still gathered into one release,
and its notes say so in their first lines.

What the player sees when the code did change:

- **An app that builds the code on the device** updates it by itself. The
  moment the update is installed the build starts in the background, with a
  notification (**Updating the game code**), from the executables the app
  kept or the disc image chosen before; it compiles only the files that came
  out different, and the game starts as before when it is done
  ([ondevice-build.md](ondevice-build.md#after-an-update-of-the-app)). It
  waits for the charger when the battery is below what the player allows
  (Settings, 30% unless changed). Opening the app meanwhile shows the
  progress in the launcher's status.
- **Game code built on a PC, on a device with the toolchain package**: the
  same; the app builds its own from the disc image and uses that.
- **Game code built on a PC and no toolchain package**: a notification and
  the launcher say that the code has to be built again, on the device
  (**Build the game code**: the toolchain is downloaded, Android asks once
  whether to install it) or with `git pull` and `setup\setup.cmd -GameOnly`
  (`bash setup/setup.sh --game-only`). Setup fetches and builds the
  recompiler again when its patch changed, recompiles the trees made with
  the old one, compiles only the files that came out different, and installs
  nothing when the phone has that code already.

When the recompiler or its patch is what changed, the build tools' package
changes with it, since the recompiler is inside: raise `TOOLCHAIN_VERSION`,
run **Build tools package**, and name the new release and hash in
`setup/toolchain.json` ([The toolchain release](#the-toolchain-release))
before the apps are released. The updated app's next build then finds the
old package and installs the new one over it itself
(`ToolchainInstall.kt`). Android asks the player about that once: the first
package the app installs. From then on the app is that package's installer,
and Android 12 and later let an installer replace its own package with a
newer one without a question, so later toolchains arrive unasked. A build
the app started by itself downloads over an unmetered network only, and
leaves a package the player never had to the player. Android takes the
newer package over the older one and never the other way: an app that was
not updated cannot go back to the older tools on a device that has the
newer ones, and builds again only once it is updated too. Its game code is
not touched by this and goes on running.

Tried on the AYN Thor with the drafted `toolchain-v2` (served as [Trying
the update without publishing](#trying-the-update-without-publishing)
says) and Modern Warfare 3: the updated app over game code built on a PC
before the change says so, **Build on this device** opens the build
screen, which finds toolchain 1 and downloads 2 over it, and the build
took 4 minutes 7 seconds for both libraries (`mw3 51df2c158616`, `mw3mp
abfe2d558ce8`); Black Tuesday then ran at 119 frames a second. On Windows,
`setup\setup.cmd -Game mw3 -GameOnly` over a folder left by the version
before fetched and built the recompiler again, recompiled both executables
and built the same two statements in a minute and a half.

What setup builds again, on Windows with Modern Warfare 2
(`setup\setup.cmd -Game mw2 -GameOnly -NoInstall`, November 2026): from
nothing, the recompiler included, 2 minutes; again with nothing changed,
18 seconds, none of it compiling (Gradle looks at the package); after a
configuration change that makes one function two, 29 seconds: the campaign
is recompiled, 104 of its 114 files come out as they were, and 10 are
compiled. A phone whose game package holds the libraries just built is
told so and gets nothing installed.

**November 2026: toolchain 2.** The recompiler keeps XER's summary overflow
in the context ([building.md](building.md#local-changes-to-xenonrecomp)):
half of Advanced Warfare's and Modern Warfare 3's functions no longer keep
every register there. The patch is in every title's statement, so all four
games' code is rebuilt once, although Modern Warfare 2's and Call of Duty
4's comes out the same. The statements before and after (the first twelve
characters, as setup and the app's log print them):

| Library | Before | After |
|---|---|---|
| mw2 | `d5de1f8e8711` | `eb135c3b4d1d` |
| mw2mp | `94e6825e2bf7` | `3110ad8a6857` |
| mw3 | `49cb73ddd14a` | `51df2c158616` |
| mw3mp | `ea9e4d15ac5e` | `abfe2d558ce8` |
| cod4 | `94ec5f58a992` | `d2625531ad9f` |
| cod4mp | `a4c71519c558` | `116e3c0c3a14` |
| aw | `c5cef5aeaede` | `2d5c2fcf4eac` |
| awmp | `6bc76440aa55` | `e269159b7302` |

Advanced Warfare's multiplayer as its title update makes it
([dlc.md](dlc.md#advanced-warfare-map-packs-and-exo-zombies)) is a library
of its own with a statement of its own, `awmp17 06fc007e86fc`, first made
with this recompiler; adding it changed no other library's statement.

## What must not reach the public repositories

The public repositories are made from this one's `android` branch with
`git filter-repo`, history and all. Two folders in that history hold build
output that was committed by mistake and removed in the next commit (the
build tools' programs, 90 MB; [ondevice-build.md](ondevice-build.md)):

    android/toolchain/build/
    android/toolchain-probe/build/

Every run of the filter has to drop both, beside what it already drops:

    git filter-repo --invert-paths --path android/toolchain/build/ --path android/toolchain-probe/build/

They are the project's own builds of free software, so nothing is disclosed
if they slip through; a mirror with them is 80 MB heavier for ever and over
GitHub's size warning. `tools/check_no_game_files.py` now refuses a tracked
APK and any tracked file over 20 MB, so the tip of a branch cannot have one;
before a push, the filtered history is checked for any blob over 20 MB too.

Advanced Warfare's source is in every mirror as code
([AW-BRINGUP.md](AW-BRINGUP.md)), and since v0.1.0 it has a repository,
a workflow and releases of its own (`yassinsolim/aw-recomp`, made as the
others are, with `README-AW.md` as its `README.md`). What is the game's own
never was in the history (`aw/`, `ppc_aw/` and `ppc_aw_mp/` are git-ignored
and refused by `tools/check_no_game_files.py`), and the filter names them
all the same:

    git filter-repo --invert-paths --path aw/ --path-glob 'ppc_aw*'

## The camo packs release

Modern Warfare 3's public repository has one more release, `camo-packs`,
which is no version of the app: its assets are the camouflage packs the
project may pass on ([textures.md](textures.md), Camouflage packs), each a
zip of the pack's folder with its `pack.json` naming the author and the
thread. `setup/camo-packs.json` names the repository, the tag, each asset
and its SHA-256; setup and the app's **Get the project's packs** download
from there, so the release has to be published for either to find it.

It is marked a pre-release and has to stay one: setup takes the app from the
repository's *latest* release, which a pre-release never is. The app's own
update check passes it over, as it has no APK.

A pack that is replaced gets its new SHA-256 in `setup/camo-packs.json`
before the app is released: an app carries the list it was built with, and
refuses a download that does not match it.

## The toolchain release

The build tools' package for **Build the game code**
([ondevice-build.md](ondevice-build.md)) is one APK for all four games, so
it has one release, in Modern Warfare 3's public repository beside the camo
packs: `toolchain-v<N>`, where N is `TOOLCHAIN_VERSION` in
`tools/toolchain/versions.sh`. `setup/toolchain.json` names the repository,
the tag, the asset (`recomp-toolchain.apk`), the version and the APK's
SHA-256; every app carries that list in its build kit, downloads from

    https://github.com/yassinsolim/mw3-recomp/releases/download/toolchain-v<N>/recomp-toolchain.apk

and hands the file to Android's installer only when its SHA-256 is the
list's. An installed package of another version is not used: the app says so
and offers the download, which replaces it.

1. In `yassinsolim/mw3-recomp`: Actions, **Build tools package**, Run
   workflow (`toolchain.yml`; about an hour, nearly all of it LLVM). It builds
   the APK from sources, refuses one not signed with the release key, and
   drafts the pre-release with `recomp-toolchain.apk`, its `.sha256`,
   `LICENSE`, `THIRD_PARTY.md`, `recomp-toolchain-licences.zip` (every notice
   the package carries) and `recomp-toolchain-recompiler-source.tar.gz`. The
   notes link each part's source at its exact commit.
2. Put the SHA-256 the run prints into `setup/toolchain.json`, and release
   the apps from that commit or a later one. An app cannot be built while the
   file has no hash.
3. Publish the toolchain release **before or with** the apps that name it:
   until then their download gets a 404 and the app shows the release page
   instead.

It is a pre-release for the camo packs' reason: setup takes the app from the
repository's latest release. A published toolchain release is never
replaced, since apps name its file by hash: what changes the package
(`versions.sh`, the recompiler's commit or patch) raises `TOOLCHAIN_VERSION`,
which is a new tag, and the workflow refuses to draft over a published one.

**The GPL.** `libXenonRecomp.so` and `libXenonAnalyse.so` include GNU
binutils' PowerPC disassembler, so they are distributed under the GPL-3.0
([THIRD_PARTY.md](../THIRD_PARTY.md)). Their complete source as compiled
(XenonRecomp at the pinned commit, submodules in, `patches/xenonrecomp-mw2.patch`
applied) is an asset of the same release, and the build scripts are this
repository at the commit the release's notes name.

## Trying the update without publishing

A debug build can be pointed at a list of releases served from the PC:

    set MW2_UPDATE_API=http://127.0.0.1:8765/releases.json
    set MW2_VERSION_CODE=10000        (and 10001 for the newer one)
    gradlew :app:assembleMw3Tu0Debug  (or :app:assembleMw2Tu0Debug, :app:assembleCod4Tu0Debug)
    python tools/serve_test_release.py newer.apk --tag v1.0.1  (--game mw2, cod4 or aw for those apps)
    adb reverse tcp:8765 tcp:8765

Install the older build, start it, and it offers the newer one. Only a debug
build may ask a server without TLS; signed again with the release key
(`apksigner sign`), it stands in for the published app, and the newer one
can then be a release's own APK.

The toolchain download is tried the same way, before its release is
published: build the debug app with

    set MW2_TOOLCHAIN_URL=http://127.0.0.1:8765/recomp-toolchain.apk

and give the server the drafted release's APK
(`gh release download toolchain-v<N> -R yassinsolim/mw3-recomp -p recomp-toolchain.apk`)
as `--toolchain recomp-toolchain.apk`. The app still takes only a file with
the SHA-256 in `setup/toolchain.json`, so this proves the hash in the list is
the draft's.
