# Building the game code on the phone

The game code is each player's own: their disc's executables, translated to
C++ by XenonRecomp and compiled for arm64 ([game-library.md](game-library.md)).
Until now that took a PC (`setup\setup.ps1`). With the build tools installed
beside the app, the phone does it: the same recompiler, the same compiler line,
the same link, and a library that states what the PC's states.

This page is the tools' side: the package `com.recomp.toolchain`, what is in
it, where it comes from and how it was tried. The app's side (finding the
executables in the disc image, running the build, loading the result) is the
app's own section below.

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
into. A compile is `compile`, the build's own flags (`-O2 -DNDEBUG -fPIC -w
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

| | AYN Thor (Snapdragon 8 Gen 2, 16 GB, Android 13) | PC (i9, 32 threads) |
|---|---|---|
| analyse and recompile | 12 s | about 10 s |
| compile | 70 to 73 s | about 50 s (`-O3 -g`) |
| link | under 1 s | |
| memory: all compilers together, at most | 1.5 GB | |
| memory: the largest one compiler | 250 MB | |
| `libmw2game.so` | 18.5 MB | 63 MB with debug information |

- The recompiled tree and the jump tables are the PC's, byte for byte (the
  three files `tools/genshared.py` writes aside, which the build writes itself).
- The library's statement is `mw2 d5de1f8e8711`: the PC build's, and what an
  app built from the checkout expects (`config/abi/mw2/`).
- The library has the PC library's dynamic section and its dynamic symbol
  table, all 55,932 symbols with their versions and bindings.
- Running the programs out of the toolchain package's library folder from
  another app, and `System.load` of a library that app had just linked into
  its own files, both work on the Thor with SELinux enforcing.

Found on the way, for whoever writes a build around the tools:

- A build must be in the foreground. A background app's processes are kept to
  three small cores (`/dev/cpuset/background/cpus` is `0-2` on the Thor).
- Ask for eight compiles by number: `nproc` says 6 in a shell on the Thor.
- The phone's shell is mksh with toybox: `hash` is a command of the shell's
  (a function of that name is never called), `grep` has no `\b`, `xargs -P`
  works.
- The debug information is what makes the PC's compile heavy (a gigabyte a
  compiler). Without it a phone of 8 GB has room for eight compilers.

