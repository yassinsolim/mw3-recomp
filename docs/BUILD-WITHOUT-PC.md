# Building your game package without a Windows PC

The Android app is published; the game's code is not. It is made from your
own disc's two executables and installed beside the app as a package of its
own ([game-library.md](game-library.md)). `setup\setup.cmd` makes it on a
Windows PC. This page is for everyone else:

- **[No PC at all](#no-pc-at-all-github-builds-it-for-you)**: your phone and
  a free GitHub account. GitHub builds the package in a **private**
  repository of your own.
- **[A Linux PC](#a-linux-pc)**: one command, `bash setup/setup.sh`.

> **New and not yet proven.** Both ways are written and the app's part is
> tested (it reads the executables out of a disc image on a Retroid Pocket
> Flip 2), but neither build has yet been run from start to finish on
> Linux or on GitHub. Expect rough edges, and please report them.

Either way you use your own disc image (`.iso`), and what is built is for
your own devices only.

In the steps below, *mw2* stands for your game: `mw3` (Modern Warfare 3),
`mw2` (Modern Warfare 2) or `cod4` (Call of Duty 4).

## No PC at all: GitHub builds it for you

You need: the phone or handheld you play on, your disc image on it, a
browser, and a [GitHub account](https://github.com/signup) (free).

### 1. Install the app and your game files

1. In the phone's browser, open the game's releases page:
   [Modern Warfare 3](https://github.com/yassinsolim/mw3-recomp/releases/latest),
   [Modern Warfare 2](https://github.com/yassinsolim/mw2-recomp/releases/latest) or
   [Call of Duty 4](https://github.com/yassinsolim/cod4-recomp/releases/latest).
2. Under **Assets**, tap the file ending in `.apk`. When the download is
   done, tap **Open** (or open it from the notification). Android asks once
   whether your browser may install apps: tap **Settings**, switch **Allow
   from this source** on, go back, tap **Install**.
3. Open the app and tap **INSTALL & PLAY** (or **Install the game files**),
   then choose your disc image. It copies the game files once (about 7 GB).
   When it is done the app says the game code is not on this device: that is
   the part you build next.

### 2. Prepare the two files

1. In the app: **Settings**, the **Files** tab, **Prepare files for building
   online**.
2. The app writes two small files into the phone's **Download** folder, for
   example `mw2-default.xex` and `mw2-default_mp.xex` (5 to 15 MB each), and
   shows these steps in short. If you skipped the game files in step 1, it
   asks for your disc image and reads the two files out of it.

These two files are the game's executables. They are yours alone: upload
them nowhere but your own private repository below.

### 3. Make your private builder

1. Sign in to GitHub in the browser.
2. Open
   [github.com/new?template_owner=yassinsolim&template_name=recomp-builder&visibility=private](https://github.com/new?template_owner=yassinsolim&template_name=recomp-builder&visibility=private).
   (Or open [yassinsolim/recomp-builder](https://github.com/yassinsolim/recomp-builder)
   and tap **Use this template**, **Create a new repository**.)
3. **Repository name**: anything, for example `my-game`.
4. Make sure **Private** is chosen. This matters: the build refuses to run in
   a public repository, and your files must never be public.
5. Tap **Create repository** and wait a few seconds for the new page.

### 4. Upload the two files

1. In your new repository tap **Add file**, then **Upload files**. On a
   narrow screen the button may be folded into the **⋯** menu beside
   **Code**; or add `/upload/main` to the end of the repository's address
   (`github.com/<your name>/my-game/upload/main`).
2. Tap **choose your files**, open **Downloads**, and pick both files (press
   and hold the first, then tap the second).
3. Wait until both are listed, then tap **Commit changes**.

The build starts by itself.

### 5. Wait for the build

Tap **Actions** (in the row with **Code**; it may be under **⋯**). **Build my
game package** is listed with a yellow dot while it runs. You can close the
browser; GitHub sends an email if it fails. A green tick means it is done.

### 6. Install the package

1. In your repository tap **Releases** (on the repository's front page,
   below the files, or add `/releases` to its address).
2. At the top is a **Draft** called **My game package**. Under **Assets**, tap
   the `.apk` (for example `mw2-game.apk`).
3. When it has downloaded, tap **Open**, then **Install**. The package has no
   icon and no screen of its own.
4. Open the game's app and play.

The package is kept as a draft on purpose: a draft can be seen by you alone.
Do not press **Publish release**.

### After an update of the app

Most updates need nothing. When one changes how the game is recompiled, the
app says the game code does not match. In your repository: **Actions**,
**Build my game package**, **Run workflow**, **Run workflow**. Then install
the new package as in step 6; your game files and saves are kept.

### What it costs

Nothing, within GitHub's allowance. A build in a private repository uses your
account's GitHub Actions minutes: a free account has 2,000 a month, and one
build of one game is expected to take one to two hours of them (GitHub's
runner for private repositories has two processors). When they are used up, GitHub does
not start the build (the run fails within seconds, saying the job was not
started because of the spending limit) and charges nothing; the allowance is
new at the start of each month. You are only ever charged if you yourself
set a payment method and a spending limit above zero.

### Keeping it private

- The repository must stay **Private**. The build stops at once in a public
  one, and looks again before it stores the package.
- Never share the two files, the package, or access to the repository.
- When you no longer need it, delete the repository: **Settings**, at the
  bottom **Delete this repository**. You can always make a new one.

### If something goes wrong

| what you see | what to do |
|---|---|
| the run fails at once: "This repository is PUBLIC" | delete the repository and start again from step 3 with **Private** |
| the run fails at once: "the job was not started because ... spending limit" | your Actions minutes for this month are used up; run it again next month (**Actions**, **Build my game package**, **Run workflow**) |
| "No executables found" | upload both files from step 2; if yours are named `default.xex` and `default_mp.xex`, use **Run workflow** and choose the game |
| "is not a disc build this port supports" | the files are from another edition of the game than the port is made for |
| Android says **App not installed** | an older game package signed with another key is installed (one built on a PC, or in another repository): uninstall it first (Android's **Settings**, **Apps**, *Modern Warfare 2 game code*), then install the new one |
| the app still says the game code does not match | the app is older than the package: let it update itself (**Check for updates**), or build for your version: **Run workflow** with the app's version (such as `v0.1.6`) in the version box |

## A Linux PC

On a 64-bit Linux (written for Ubuntu 24.04), with about 25 GB free:

```sh
git clone https://github.com/yassinsolim/mw2-recomp.git && cd mw2-recomp
bash setup/setup.sh --game mw2 --iso ~/Downloads/MW2.iso
```

It fetches a JDK and the Android SDK and NDK into `.toolchain/` (pinned and
checked; nothing installed system-wide), reads the two executables out of
your disc image (read-only), checks them, recompiles them, builds
`out/mw2-game.apk`, downloads the published app, and offers to install both
on a phone connected over USB with USB debugging on. A few programs have to
be the system's own; it names the ones missing and offers to install them:

```sh
sudo apt install git curl unzip python3 openssl clang
```

(`clang` 17 or later builds the recompiler.)

| option | what it does |
|---|---|
| `--check` | read-only: says what is present and what would be fetched |
| `--install`, `--device <serial>` | install without asking; which phone, when several are connected |
| `--push-iso` | also copy the disc image to the phone's Download folder |
| `--game-only` | build and install the game code only: what to run after `git pull` when the app says the game code does not match |
| `--xex-dir <folder>` | take the two executables from a folder instead of a disc image |
| `--jobs <n>` | how many compilers run at once |
| `--dev`, `--suffix .dev` | build the app here instead of downloading it; under another package name |
| `--yes` | ask nothing |

Running it again is safe: finished steps are skipped.

**An old PC, or little memory.** The recompiled code is a few hundred very
large files, and each compiler working on one takes up to about 1.5 GB.
Setup counts the free memory and runs only as many compilers as fit
(`--jobs 1` forces one). With under 3 GB free it warns: close the browser
first, and add a swap file if the build is killed. The build then takes
longer, not more memory; at its end setup says how long it took and how
much memory was in use at its fullest.

The Windows game and other authors' camouflage packs are not part of the
Linux script: the first is `setup\setup.ps1`'s, the second the app fetches
itself (**Settings**, **Multiplayer**, **Camo packs**).
