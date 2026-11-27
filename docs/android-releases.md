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

### The automated procedure

`tools/release/release.py` does the steps above and refuses to go on when a
gate fails, saying which. The owner has approved publishing without being
asked each time, on the condition that every gate passes; nothing is
published by any step but the last.

    set RELEASE_WORK=C:\Users\solim\mw3-android-work
    python tools/release/release.py status          what is published, what comes next
    python tools/release/release.py sync --push     filter the branch into the four mirrors, check, push main
    python tools/release/release.py tag             push the next tags: the workflows draft the releases
    python tools/release/release.py withdraw        take the tags and drafts back (a later commit is to be released; never a published one)
    python tools/release/release.py gates --wait    gates 1 to 4 and 7 on the drafts
    python tools/release/release.py published-apk   the previous published APK, from GitHub Releases, with its SHA-256 (gate 6's old app)
    python tools/release/release.py device-template > report.json
    python tools/release/release.py publish --device-report report.json
    python tools/release/release.py verify-public   (publish runs it too)

`RELEASE_WORK` holds the mirrors' clones (`public-hist`, `mw2-public`,
`cod4-public`, `aw-public`, each with the remote `public`; cloned when
missing), the Android SDK and `out/release`, where the drafts' files, the
state and the gates' report are kept. The next tag is the published one's
patch plus one unless `--version mw3=v0.2.0` says otherwise; `--games`
releases some of the four. The notes are written by hand beforehand, as
`docs/releases/<game>-<tag>.txt`.

The gates. All of them pass or nothing is published:

1. **CI.** Every workflow on the mirror's new main (`tests`, `MW3
   validation`) and the tag's release workflow ended well. Before the push,
   in each filtered mirror: `tools/check_no_game_files.py` passes, no blob in
   the whole history is over 20 MB, no path that the filter drops, no game
   file and no path of a game that is not released (`bo3`) is anywhere in
   it, and the public main is an ancestor of the filtered branch, so the
   push is a fast-forward. It is never forced.
2. **The APK.** `tools/check_runtime_apk.py`: no recompiled function body
   and no game file; signed with the release key
   (`RELEASE_CERTIFICATE_SHA256`); the package and the version code are the
   tag's.
3. **The driver.** Inside the APK itself, not by the build's log: the Turnip
   package with the SHA-256 `android/fetch_deps.sh` names, with a driver in
   it; libadrenotools' four hook libraries; and each title's library built
   with libadrenotools. Without them the app runs on the phone's own
   driver, whose picture on an Adreno is wrong.
4. **The game code.** Every statement the published app took
   (`<game>-recomp-<tag>-game-abi.txt` of the published release) is taken by
   the draft, so nobody builds anything; or the notes' first lines say that
   the game code is built again (the app then builds it by itself,
   [When the game code has to be rebuilt](#when-the-game-code-has-to-be-rebuilt)).
   The build tools' release `setup/toolchain.json` names exists with that
   SHA-256; when it is still a draft it is published before the apps.
5. **The devices**, not the script's to pass: on the AYN Thor and the
   Retroid Pocket Flip 2, each game is started, a campaign level and a match
   with bots are played for two to three minutes, the visual suite passes
   ([visualqa.md](visualqa.md)), `game.log` names Turnip, and the frame rate
   is the device's (about 115 to 120 on the Thor, 60 on the Flip 2) and no
   lower than the published release's beyond noise.
6. **The update**, on a device as well, **as a player has it**: the
   previous *published* APK (downloaded from GitHub Releases) builds the game
   code on the device, then the release-signed new APK is installed over it
   through Android's package installer (`pm install -r` from a file on the
   device, which is what a downloaded APK goes through), and the game code
   built on the device is still accepted with no rebuild, or is built again
   by itself when the notes say so. The saves and the settings are kept.
   [The update as a player does it](#the-update-as-a-player-does-it) says
   how and how long; the old fast check stays as its pre-check.
7. **The notes.** Plain text, under 1,450 characters (the app shows 1,500),
   written for players, naming the tag, with a `Known issues` line.

Gates 5 and 6 are tried by hand or by an agent, through the devices' lock,
and written into the report `device-template` prints: for each game and
device `campaign`, `multiplayer`, `visual`, `turnip` and `fps`, and for each
game `update`, each with `pass` and the folder or file of its evidence.
`publish` refuses a report with anything not passed or without evidence.

**What stands in for the draft's APK on the devices.** A draft's APK has the
published app's package name and key, so installing it would replace the
owner's app and touch its data; and a release build reads no `options.txt`
and is not debuggable, so nothing can be scripted in it or written into its
preferences. What is tried instead is the mirror's tagged commit built here
three ways under a package suffix (`-PappSuffix=.rc`): the release variant
signed with the release key (the workflow's own Gradle task and
environment), for the start and the play by hand (and for the update when
the device has the owner's app: see [the update as a player does
it](#the-update-as-a-player-does-it)); the debug
variant signed again with the release key, installed first so that `run-as`
can write the preferences of a set-up app, and replaced by the release
variant, which keeps them; and the debug variant under `.vq` for the visual
suite. The published version for gate 6 is built the same way from its own
commit with its own version code. The report says so in `equivalent`.

`publish` runs gates 1 to 4 and 7 again on the drafts as they are then,
checks the report, publishes the build tools first when they are a draft,
then each game (notes from the file, marked latest), and ends with
`verify-public`: each release's APK and statements are fetched by the
addresses the app and setup use (`releases/download/<tag>/` and
`releases/latest/download/`), the APK's SHA-256 is the one the gates passed,
each repository's latest release is the new tag, and the build tools'
download has the SHA-256 named.

## The update as a player does it

Gate 6 used to be `adb install -r` of two locally built `.rc` copies
over game code that came from a PC. That is not what a player does: the
player's game code is built *on the phone* by the published app, and the next
app arrives as a downloaded APK handed to Android's package installer. The
tester's log (Advanced Warfare, v0.1.1 to v0.1.4) shows that path works, but
a gate that never took it could not have shown otherwise. Gate 6 is now:

1. **Pre-check, kept** (a few minutes): the old fast check. The new `.rc`
   release build over the old one with `adb install -r`, on game code from a
   PC (`setup\setup.cmd`): the statements are taken as they are, or the
   build starts by itself when the notes say it must. It catches a broken
   statement, a refused library or lost preferences before the long run.
2. **The old app is the published one.** `release.py published-apk` downloads
   `<game>-recomp-<published tag>.apk` from GitHub Releases, checks it as gate 2
   does and prints its SHA-256; the report names that hash
   (`update.from_apk_sha256`), and `publish` compares it with the file.
   Install it with `adb install` (a first install, so the installer's path
   does not matter yet), choose the disc image, and **build the game code on
   the device** with it (every library the game has, and the title update's
   when Add DLC has been done): wait for the notification to end. Or reuse a
   device on which that build already stands. The report's **game code**
   line then reads `was built on this device for <hash>, which this app
   takes` for each library, the title update's too (`libawmp17game.so`).
3. **The new app goes over it as a downloaded APK does.** The draft's APK is
   copied to the device and installed from there, with the game closed or
   running:

       adb push aw-recomp-vX.Y.Z.apk /data/local/tmp/new.apk
       adb shell pm install -r /data/local/tmp/new.apk

   (`installed_with` in the report is `pm install -r`.) The draft's APK has
   the published package name and key, so this takes the real package: only
   on a device that does not have the owner's app of that game, or whose
   owner agrees to it, since the draft's version cannot be taken back.
   Where the owner's app is on the device, the same is done under the
   package suffix: the previous version built from its commit (its own
   version code, `-PappSuffix=.rc`, the release variant signed with the
   release key) takes the place of the downloaded APK, builds the game code
   on the device, and the new `.rc` release build is put over it with the
   same `pm install -r` from a file; the report names it in `test_package`
   and says in `equivalent` what stood for the published APK. Never
   `adb uninstall`, `pm clear` or a reinstall on the way: the data must
   survive the update, as it does for a player.
4. **What has to be seen**: the launcher still says the game is ready, the
   report's game-code line is the same as before the update (every library
   built on this device for a statement this app takes) and no build ran
   (`game_code_rebuilt: false`), or, when the notes' first lines say the game
   code is built again, the build starts by itself and ends (`true`); the
   saves and the settings are as they were; a campaign level starts and loads
   `files/game-code/lib<name>game.so`. The report's `game_code` is that line,
   and `publish` refuses it with MISSING in it, or with "a rebuild is due"
   when the notes promise none.

**How long it takes**, per game and device: the old app's build is the long
part, and is [measured](ondevice-build.md#pausing-interruptions-memory-and-heat)
at 2 to 4 minutes for Call of Duty 4 and 4 to 8 for Modern Warfare 3 on the
Thor and the Flip 2, and 12 to 13 minutes for Advanced Warfare (plus 6 for
the update's library after Add DLC) on the Flip 2. The install and the
checks add about 5 minutes (an estimate, not yet timed), so 10 to 30 minutes
a game and device, and for all four games on two phones, one after another
through the devices' lock, a few hours. A build that already stands on a test
package is reused for later releases that have the same previous version:
only the install and the checks are left.

**Tried on the AYN Thor** (27 November 2026, the release-signed `.ug` apps
that `tools/release/update_test_apks.sh` builds at the two tagged commits,
because the published APK cannot be renamed; the disc images from Downloads;
`pm install -r` of the new APK from `/data/local/tmp` while the game ran):

| Game | From, to | Build on the device (old app) | Install | After the update |
|---|---|---|---|---|
| Call of Duty 4 | v0.1.5, v0.1.6 | 2 min 14 s | 0.5 s | both libraries "built on this device", no build service, `game-build-report.txt` untouched, the game ran at 119 frames a second |
| Modern Warfare 2 | v0.1.10, v0.1.11 | 3 min 2 s | 0.5 s | the same; `files/game-code/libmw2game.so` loaded |
| Modern Warfare 3 | v0.1.10, v0.1.11 | 4 min 30 s | 0.5 s | the same; `libmw3game.so` loaded |
| Advanced Warfare | v0.1.3, v0.1.4 | 6 min 48 s (campaign and multiplayer), then Add DLC (11 packages and the title update) built the update's library in 3 min 30 s | 0.5 s | `libawgame.so` and `libawmpgame.so` "built on this device", "11 packages and the title update" still there, `libawgame.so` loaded, no build service, `game-build-report.txt` (the `awmp17` build) untouched. The v0.1.3 app's log line does not list `libawmp17game.so` (the gap this change closes), so its acceptance is shown by no rebuild having started, not by a line |

A test app of Modern Warfare 3 built without the workflow's
`MW3_XEX_SHA256` refuses the player's disc ("not a disc this build was made
from"): the script sets it.

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
