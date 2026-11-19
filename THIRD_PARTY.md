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
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | fetched by CMake on Windows | Apache-2.0 / MIT |
| FFmpeg, [has207's branch](https://github.com/has207/FFmpeg) with Xenia's XMA decoder | fetched by `third_party/ffmpeg-xenia/CMakeLists.txt`; the configuration headers and codec lists there are Xenia's | LGPL-2.1-or-later (FFmpeg); BSD-3-Clause (Xenia's files) |
| [RenderDoc](https://github.com/baldurk/renderdoc) in-application API header | `third_party/renderdoc/renderdoc_app.h` | MIT (in the file) |
| Barlow fonts | `third_party/fonts/Barlow*.ttf` | SIL OFL 1.1 (`third_party/fonts/OFL.txt`) |
| Michroma font | `third_party/fonts/Michroma-Regular.ttf` | SIL OFL 1.1 (`third_party/fonts/Michroma-OFL.txt`) |
| Zone structure definitions from [OpenAssetTools](https://github.com/Laupetin/OpenAssetTools) via [pc2xbox360mw2ools](https://github.com/slanginbeans/pc2xbox360mw2ools) | `launcher/zone/defs/`, `launcher/zone/defs_pc/` (each has a `NOTICE.md`) | GPL-3.0 |
| [libadrenotools](https://github.com/bylaws/libadrenotools) | fetched into `third_party/libadrenotools/` | BSD-2-Clause |
| Mesa Turnip driver | fetched, see below | MIT |

## Mesa Turnip (Vulkan driver for Qualcomm Adreno)

The Modern Warfare 3 Android app (`com.mw3.recomp`) and its multiplayer test
copy (`com.mw3.recomp.mp2`) use
a build of the Mesa Turnip driver as their default graphics driver on
Qualcomm devices.

- Project: Mesa, <https://gitlab.freedesktop.org/mesa/mesa> (Turnip is its open
  source Vulkan driver for Adreno GPUs)
- Licence: MIT
- Build used: Turnip v27.0.0-R2 by StevenMXZ,
  <https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/download/v39/Turnip_v27.0.0-R2.zip>
  (SHA-256 `8d30be022582d0c03b1918c019540491d00439e14e9e5e1359fe0bb19004f781`)

The binary is downloaded at build time by `android/fetch_deps.sh` (or
`setup/setup.ps1` on Windows), checked against the hash above and packaged
into the apps' assets. It is not stored in this repository.

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

## Game data

None. Modern Warfare 3 and its executables are Activision's; the player
supplies a disc image of their own copy, and nothing made from it (extracted
files, the decrypted image, generated C++, APKs, executables) is ever committed
or published. `tools/check_no_game_files.py` checks every tracked file.
