# recomp-builder: build your game package without a PC

A template for players of [MW3 Recompiled](https://github.com/yassinsolim/mw3-recomp),
[MW2 Recompiled](https://github.com/yassinsolim/mw2-recomp) and
[CoD4 Recompiled](https://github.com/yassinsolim/cod4-recomp) on Android who
have no PC to build the game code on. GitHub builds it for you, in a
**private** repository of your own, from the two executables of **your own
disc**. All you need is your phone and a free GitHub account.

The whole guide, tap by tap:
[docs/BUILD-WITHOUT-PC.md](https://github.com/yassinsolim/mw3-recomp/blob/main/docs/BUILD-WITHOUT-PC.md).
In short:

1. In the game's app: **Settings → Prepare files for building online**. It
   writes two small files into your phone's Download folder, such as
   `mw2-default.xex` and `mw2-default_mp.xex`.
2. Here: **Use this template → Create a new repository**, and choose
   **Private**.
3. In your new repository: **Add file → Upload files**, pick the two files,
   **Commit changes**. The build starts by itself (**Actions** shows it) and
   takes an hour or two.
4. When it is done: **Releases**, the draft **My game package**, tap the
   `.apk`, install it. Open the game's app.

## Keep it private

The executables and the package contain the game's code. They are yours
alone: **never make your copy of this repository public, and never share the
package.** The workflow refuses to run in a public repository, and keeps the
package as a draft release that only you can see. This template itself
contains nothing of any game.

The workflow is one file,
[.github/workflows/build-game-package.yml](.github/workflows/build-game-package.yml);
its source is kept in the games' repositories under `setup/recomp-builder/`.
