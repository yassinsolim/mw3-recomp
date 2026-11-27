# Third-party software

This repository is licensed under the GPL-3.0-only ([LICENSE](LICENSE)), as
are the projects it derives from: [mw2-recompiled](https://github.com/PaulCombal/mw2-recompiled),
[mw3recomp](https://github.com/the-drk/mw3recomp) and
[mw2-android](https://github.com/OpenPort-Labs/mw2-android). The components
below keep their own licences. Those marked *fetched* are downloaded at build
time and are not stored here.

| component | where | licence |
|---|---|---|
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (and its submodules) | fetched into `XenonRecomp/` by `build.sh`, `tools/recomp_mw3.sh` and `setup/setup.ps1`; our changes are `patches/xenonrecomp-mw2.patch` | MIT |
| [SDL 3](https://github.com/libsdl-org/SDL) | fetched by CMake | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) | fetched by CMake (desktop launcher) | MIT |
| [zlib](https://github.com/madler/zlib) | fetched by CMake | zlib |
| [gsc-tool](https://github.com/xensik/gsc-tool) by xensik: the GSC compiler and the Xbox 360 IW5 tables (instructions, built-in functions and methods, names) behind the Modern Warfare 3 mods folder | fetched by CMake at a pinned commit; used from `runtime/mods_gsc.cpp` | GPL-3.0 |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | fetched by CMake on Windows | Apache-2.0 / MIT |
| FFmpeg, [has207's branch](https://github.com/has207/FFmpeg) with Xenia's XMA decoder | fetched by `third_party/ffmpeg-xenia/CMakeLists.txt`; the configuration headers and codec lists there are Xenia's | LGPL-2.1-or-later (FFmpeg); BSD-3-Clause (Xenia's files) |
| [RenderDoc](https://github.com/baldurk/renderdoc) in-application API header | `third_party/renderdoc/renderdoc_app.h` | MIT (in the file) |
| Barlow fonts | `third_party/fonts/Barlow*.ttf` | SIL OFL 1.1 (`third_party/fonts/OFL.txt`) |
| Michroma font | `third_party/fonts/Michroma-Regular.ttf` | SIL OFL 1.1 (`third_party/fonts/Michroma-OFL.txt`) |
| Zone structure definitions from [OpenAssetTools](https://github.com/Laupetin/OpenAssetTools) via [pc2xbox360mw2ools](https://github.com/slanginbeans/pc2xbox360mw2ools) | `launcher/zone/defs/`, `launcher/zone/defs_pc/` (each has a `NOTICE.md`) | GPL-3.0 |
| [FidelityFX Super Resolution 1.0](https://github.com/GPUOpen-Effects/FidelityFX-FSR) by Advanced Micro Devices: its RCAS sharpening filter, written out as GLSL | `runtime/gpu/vulkan/shaders/sharpen.frag` (and its SPIR-V in `sharpen_shaders.inc`) | MIT (in the file) |
| [libadrenotools](https://github.com/bylaws/libadrenotools) | fetched into `third_party/libadrenotools/` | BSD-2-Clause |
| Mesa Turnip driver | fetched, see below | MIT |

## Mesa Turnip (Vulkan driver for Qualcomm Adreno)

The Modern Warfare 3 Android app (`com.mw3.recomp`), its multiplayer test
copy (`com.mw3.recomp.mp2`) and the Modern Warfare 2 apps
(`cod.modern.warfare2.sp`, `.mp`) use
a build of the Mesa Turnip driver as their default graphics driver on
Qualcomm devices.

- Project: Mesa, <https://gitlab.freedesktop.org/mesa/mesa> (Turnip is its open
  source Vulkan driver for Adreno GPUs)
- Licence: MIT
- Build used from Android 11 on: Turnip v27.0.0-R2 by StevenMXZ,
  <https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/download/v39/Turnip_v27.0.0-R2.zip>
  (SHA-256 `8d30be022582d0c03b1918c019540491d00439e14e9e5e1359fe0bb19004f781`)
- Build used on Android 10: the project's own, of Mesa at commit
  `483aff363be4d6913017b834123c8eeaada171d9`, unmodified, compiled with NDK
  r28c for API 29 by `android/build_turnip.sh` (every community build needs
  Android 11's libc)

The first is downloaded at build time by `android/fetch_deps.sh` (or
`setup/setup.ps1` on Windows), checked against the hash above and packaged
into the apps' assets; the second is built by `android/fetch_deps.sh`, which
runs the script. Neither is stored in this repository.

Mesa's licence, MIT, as it applies to the Turnip sources both builds are made
from (each file names its own copyright holders; the few files under other
permissive licences are listed in Mesa's `licenses/` directory):

```
Copyright © the Mesa authors

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

## libadrenotools

Loads a driver the player imported (or the one above) without root.
<https://github.com/bylaws/libadrenotools>, fetched by `android/fetch_deps.sh`
(or `setup/setup.ps1`) into `third_party/libadrenotools/` under its own
licence.

## Build tools

`setup/setup.ps1` downloads, into the git-ignored `.toolchain/`, tools that
are used to build but are not part of what is built: Eclipse Temurin JDK 17
(GPL-2.0 with Classpath Exception), the Android SDK, NDK and platform tools
(Android Software Development Kit License Agreement, accepted by the person
running setup), LLVM 18 (Apache-2.0 with LLVM exception), llvm-mingw
(Apache-2.0 with LLVM exception and the mingw-w64 licences), CMake
(BSD-3-Clause), Ninja (Apache-2.0), and when missing, Python (PSF) and
Portable Git (GPL-2.0).

## The build tools' package (`com.recomp.toolchain`)

The package that builds the game code on the phone
([docs/ondevice-build.md](docs/ondevice-build.md)) is published, so everything
in it is there under a licence that allows it, with its notice in the
package (`assets/toolchain/data.zip`, `licences/`). It is built from sources
by `tools/toolchain/build.sh`; none of it is stored in this repository.

| in the package | from | licence |
|---|---|---|
| clang and lld (`libllvm.so`) | [LLVM](https://github.com/llvm/llvm-project) 19.1.7 release sources (SHA-256 in `tools/toolchain/versions.sh`) | Apache-2.0 with LLVM exception |
| libc++ and libc++abi (headers and static libraries), libunwind, the compiler's runtime (`libclang_rt.builtins`, `libatomic`) and its headers | the same LLVM sources | Apache-2.0 with LLVM exception (libc++, libc++abi and libunwind also MIT/NCSA) |
| Android's C library headers | [AOSP bionic](https://android.googlesource.com/platform/bionic), `libc/include`, at the commit in `tools/toolchain/versions.sh` | BSD-style, several holders (`libc/NOTICE`) |
| the kernel's interface headers (`linux/`, `asm/`, `asm-generic/` and the like) | bionic's `libc/kernel/uapi`, which is the Linux kernel's user-space interface with the comments removed | GPL-2.0 with the Linux syscall note: a program compiled against them is not bound by the GPL |
| `libc.so`, `libm.so`, `libdl.so` as a link sees them: empty libraries with bionic's lists of exported names | generated from bionic's `libc.map.txt`, `libm.map.txt`, `libdl.map.txt` by `tools/toolchain/ndk_stubs.py` | BSD-style (libc, libm), Apache-2.0 (libdl) |
| `crtbegin_so.o`, `crtend_so.o` | compiled from bionic's `libc/arch-common/bionic` | BSD-2-Clause |
| XenonRecomp and XenonAnalyse (`libXenonRecomp.so`, `libXenonAnalyse.so`), `ppc_context.h` | [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) at the pinned commit with `patches/xenonrecomp-mw2.patch` | MIT; our patch GPL-3.0-only. Linked into the programs: fmt (MIT), toml++ (MIT), xxHash (BSD-2-Clause), tiny-AES-c (Unlicense), TinySHA1 (permissive, in the file), libmspack (LGPL-2.1) and the PowerPC disassembler of GNU binutils (GPL-2.0-or-later), so the two programs as a whole are distributed under the GPL-3.0, with this repository as their source |
| [SIMDe](https://github.com/simd-everywhere/simde) headers | XenonRecomp's submodule | MIT |

**Not the Android NDK.** The NDK is downloaded under the Android Software
Development Kit License Agreement, which forbids redistributing it except for
its open-source parts under their own licences. Rather than argue which files
those are, the package takes nothing from the NDK's files: the headers and
the lists of names come from AOSP's own repository, the libraries are built
from LLVM's and bionic's sources. The NDK is used as the compiler that builds
them, as it builds the app; what its compiler links into a program it builds
(its libc++ and runtime, in `libllvm.so` and the two recompiler programs) is
LLVM's code under LLVM's licence. Termux's packages were the other candidate:
its `ndk-sysroot` is the NDK's sysroot repackaged, and its clang expects
Termux's own folder and a dozen shared libraries with versioned names, which
Android does not unpack from a package.

## Game data

None. Modern Warfare 3 and its executables are Activision's; the player
supplies a disc image of their own copy, and nothing made from it (extracted
files, the decrypted image, generated C++, APKs, executables) is ever committed
or published. `tools/check_no_game_files.py` checks every tracked file.
