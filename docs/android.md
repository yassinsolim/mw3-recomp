# Android Port Documentation

This document describes the architecture, hardware compatibility, build pipeline, and runtime configuration for running **Modern Warfare 2 (2009)** natively on Android.

---

## 1. Architecture Overview

Rather than running through a traditional emulator (which introduces heavy instruction translation overhead), the Android port uses ahead-of-time compiled C++ generated directly from the Xbox 360 executable:

```
Xbox 360 default.xex  --->  XenonRecomp  --->  Recompiled C++ (ppc/)
                                                     |
                                            Android NDK (Clang 18)
                                                     |
                                                libmw2.so
                                                     |
               +-------------------------------------+------------------------------------+
               |                                                                          |
    Launcher Process (UI)                                                      Game Process (:game)
  - MainActivity.kt (Kotlin)                                                 - GameActivity.kt
  - Disc discovery & installation                                            - Native Vulkan ANativeWindow
  - Custom driver selection (Turnip)                                         - Dedicated game worker thread
  - Global preferences                                                       - Low-latency AAudio engine
                                                                             - Virtual touch gamepad overlay
```

### Dual-Process Isolation
The app is structured into two separate processes in the Android manifest:
1. **The Launcher Process** (`com.mw2.recomp.sp`): Renders the setup, preferences, driver installer, and log viewer. It stays alive independently of the game run.
2. **The Game Process** (`com.mw2.recomp.sp:game`): Hosts the recompiled guest runtime and Vulkan presentation context. Because guest Xbox 360 threads cannot be gracefully unwound mid-frame without risking driver faults, exiting the game terminates the `:game` process cleanly without crashing the launcher.

---

## 2. Hardware Compatibility & GPU Requirements

### System Requirements
- **Architecture**: 64-bit ARM (`arm64-v8a`). 32-bit devices are strictly unsupported due to the guest's 4 GiB virtual address space reservation.
- **Operating System**: Android 10 or later (API level 29+). Fully compatible with 16 KB memory page kernels on Android 14 and 15.
- **RAM**: Minimum 4 GB RAM (6 GB+ recommended).
- **Storage**: ~7-8 GB free internal storage for game assets.

### GPU & Vulkan Driver Requirements
The custom Vulkan pipeline renderer requires:
- **Vulkan 1.3** core support, OR
- **Vulkan 1.1 / 1.2** with both `VK_KHR_dynamic_rendering` and `VK_EXT_extended_dynamic_state` extensions.

#### Compatibility by SoC / GPU Architecture

| SoC Family | GPU Architecture | Compatibility Status | Notes |
| **Qualcomm Snapdragon (Flagship & Mid-Range)** | Adreno 6xx, 7xx (SD 865–8 Gen 3, **SD 778G / 778G+**, 782G, 7 Gen 1/2/3, 7+ Gen 2/3) | **Supported (100% Native)** | Vulkan 1.3 supported out of the box via stock drivers or sideloadable Mesa **Turnip** drivers (`Adreno 642L` on 778G+ has complete Turnip support). Solid 60 FPS at 720p/1080p. |
| **Qualcomm Snapdragon (Entry Mid-Range)** | Adreno 619, 710 (SD 695, SD 6 Gen 1, SD 4 Gen 2) | **Supported (30–60 FPS)** | Supports Turnip and stock Vulkan 1.1/1.3. Playable with 60%–75% resolution scale. |
| **Google Tensor** | Mali-G78, Mali-G710, Immortalis-G715 (Pixel 6, 7, 8, 9) | **Supported (100%)** | Google ships native Vulkan 1.3 drivers on Android 13/14+. |
| **Samsung Exynos** | AMD RDNA2 / RDNA3 Xclipse (Exynos 2200, 2400) | **Supported (100%)** | Native Vulkan 1.3 driver support. |
| **MediaTek Dimensity (Flagship)** | Mali-G710, Immortalis-G720 (Dimensity 8100, 8200, 9000, 9200, 9300) | **Supported (100%)** | Native Vulkan 1.3 driver support. |
| **MediaTek Dimensity (Mid-Range)** | Mali-G57, Mali-G68 (Dimensity 700, 810, 920, 1080, 6080, 7050) | **Supported (100% Playable)** | Fully supported via dedicated Vulkan 1.1 `VkRenderPass` legacy mode (`runtime/gpu/vulkan/render_pass.cpp`) with non-blocking shader warmup, bypassing OEM lack of `VK_KHR_dynamic_rendering`. |
| **MediaTek Helio (Budget)** | Mali-G52, Mali-G57 MC2, Mali-G76 (Helio G99, G96, G95, G88) | **Supported (Playable 30–60 FPS)** | Supported via Vulkan 1.1 `VkRenderPass` fallback + CPU BC1–BC5 texture decompressor. Playable at 540p–720p 60% scale. |

---

## 3. SoC Tuning & Performance Optimization

Modern mobile SoCs use heterogeneous CPU clusters (e.g., 1 Prime + 3 Gold + 4 Silver, or 2+2+4 on Tensor). Left to the default OS scheduler, critical guest threads can be scheduled onto low-frequency efficiency cores.

### Core Affinity (`runtime/platform.cpp`, `runtime/android/perf.cpp`)
- At startup, the runtime queries `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq` to identify the fastest CPU cores.
- The ring consumer, which parses every draw of a frame and is the one stage the title's render thread waits on, is pinned to the prime core and has it to itself. The title's main thread, the guest threads, the command recorder and the texture readers are pinned to the performance cores; a thread takes the cores of the thread that started it, so the presenter, the guest's blank and the audio worker are there too. (With the main thread on the prime core they all were, and the consumer got two thirds of it: 61.7 against 74.4 FPS in Survival on the AYN Thor at 120 Hz.)
- Efficiency cores (Cortex-A55 / A510) are reserved for background OS services and audio downmix processing.

### Dynamic Memory Management & Texture Budget
- **Device-Aware Defaults** (`ApplyMemoryDefaults`, `runtime/android/perf.cpp`): on Unified Memory Architecture the CPU and GPU share one pool, and the desktop defaults are sized for a machine with a separate one. Each default is taken from the memory this phone has, and each is set with overwrite off, so the launcher's own setting or a variable already in the environment wins:
  - **Texture budget**: a sixth of the device's memory, floor **128 MB**, capped at 1 GB and 1.5 GB at 12 GB — not the desktop "half of device-local RAM", which on a 6 GB phone is most of the phone. RSS stays in the low hundreds of megabytes in active gameplay.
  - **Upload arena** (`MW2_ARENA_MB`): committed up front, so **384 MB** at 8 GB and up, **256 MB** at 6 GB, **192 MB** below. The arena has a floor — a frame that runs out of its share draws nothing from the point it ran out — so unlike the shadow it is not taken below what a heavy frame needs.
  - **Guest-memory shadow** (`MW2_NO_SHADOW`): 512 MB behind the arena, in the same buffer, and a cache rather than a requirement — without it a draw copies its vertex data again, which is slower and nothing else. On a phone of **6 GB or less** the runtime does without, so nothing asks for a gigabyte before the first frame is drawn.
- **16 MB Memory Slabs (`image_memory.h`)**: Desktop builds allocate texture images in 64 MB blocks (`kBlockBytes`). On Android, block size is reduced to **16 MB** (`16ull << 20`) to eliminate oversized allocations, minimize heap fragmentation, and reduce virtual memory address space pressure.
- **Two-Pass LMK-Safe Eviction Engine**:
  - The cache inspection interval runs every 32 frames (reduced from 64) with an idle threshold `kUnusedFrames = 120` (2 seconds of inactivity).
  - **Pass 1 (Normal Sweep)**: Evicts textures that have been idle and unbound beyond the grace window down to 75% of budget.
  - **Pass 2 (Emergency Sweep)**: If streaming bursts still exceed the target threshold, an emergency pass evicts textures unbound for just 16 frames (outside the active GPU ring buffer depth). This prevents memory spikes during rapid mission transitions and high-density streaming (e.g. S.S.D.D. obstacle course).
- **`onTrimMemory` Responsive Budgeting**: When the OS sends low-memory warnings (`TRIM_MEMORY_RUNNING_LOW` or `TRIM_MEMORY_RUNNING_CRITICAL`), the texture cache automatically quarters its budget and calls `mallopt(M_PURGE, 0)` to return freed memory pages to the kernel.
- **Restoration**: When returning from background, the texture budget is dynamically restored to full capacity.

### Staying Alive in the Background (`GameService.kt`)
A run is the largest thing on the phone: the guest's 512 MB bank, the upload arena and the texture cache. Stepping out to a message stops the activity, and a stopped activity's process drops to Android's cached bucket, where it can be frozen and then killed — and the whole load starts again. `GameService` is a foreground service declared in the `:game` process with type `specialUse`, so it holds the run's process above the low-memory killer for as long as the game is up, and its notification says why it is there. (Created on Android 8+ only; Android 14 takes a foreground service only for a type the manifest names, hence `specialUse` with its subtype property.)

It does not keep the game *running* while it is away: the activity still pauses the guest when it goes off screen (`nativeSetPaused`). Nor does it license holding memory the system wants back — `onTrimMemory` still quarters the texture cache under pressure and `onResume` still restores it, service or no service.

### Mobile GPU Compatibility & Decompression
- **Zero-Copy BC1–BC5 CPU Software Decoder**: Mobile GPUs such as ARM Mali lack hardware BC/DXT block compression support. The runtime includes an optimized CPU decompressor (`runtime/gpu/vulkan/bc_decoder.cpp`) utilizing direct 32-bit and 64-bit aligned word writes, eliminating per-block `std::memcpy` call overhead. It decompresses BC1/BC2/BC3/BC5 into uncompressed RGBA8/RG8 buffers while calculating dirty checksums on original guest bytes to avoid infinite re-staging loops.
- **Silhouette & Normal Map Lighting Fix**: Unbound fallback samplers are seeded with neutral opaque white texels (`(255, 255, 255, 255)`) rather than transparent black (`(0, 0, 0, 0)`), preventing dark silhouette artifacts on character models, gear, weapons, and foreground geometry when ambient or lighting tables are streaming in. DXN / BC5 two-channel normal maps are appropriately swizzled to match mobile fragment pipelines.

---

## 4. Persistent Vulkan Pipeline & Shader Caching

Compiling SPIR-V shaders and graphics pipelines on mobile drivers causes noticeable in-game frame micro-stutters. The Android port implements a multi-tier caching and non-blocking warmup architecture:

- **Location**: Shader microcode recipes and driver binary pipeline caches are persisted in app-specific external files storage:
  `/sdcard/Android/data/com.mw2.recomp.sp/files/shader_cache.bin`
- **Instant Launcher-to-Title Boot**:
  On startup, `PrewarmFromCache()` operates under a 15-pipeline budget (or 250 ms max deadline). The initial 15 critical title-screen pipelines compile in ~200 ms, presenting the "PRESS START BUTTON" screen at locked 60 FPS within 2 seconds.
- **Background Warmup Scheduler (`warmup::Worker`)**:
  All remaining gameplay pipelines are queued into `warmup::g_warmupQueue` and compiled asynchronously on a dedicated background thread (`tid 25205`) during menus and cutscenes without dropping frame rate.
- **Auto-Flush on Pause / Lifecycle Changes**:
  Whenever the surface is destroyed, the user pauses, or memory is trimmed, `vk::pipeline::SaveCache()` commits the driver's native pipeline state blobs alongside the shader recipes.
- **Signal Safety & Crash Guarding (`runtime/crash.cpp`)**:
  Bionic libc lacks `pthread_kill` validation for arbitrary guest thread IDs. Long-running compilation stages are protected with Android-specific signal guards (`#ifndef MW2_ANDROID`), preventing spurious `SIGABRT` crashes.

---

## 5. Controls & Virtual Gamepad

### Virtual Touch Overlay (`TouchOverlayView.kt`)
The virtual gamepad converts multi-touch gestures into normalized synthetic XInput controller packets:
- **Movement (Left Stick)**: Analog thumbstick with customizable deadzone.
- **Camera Look (Right Side)**: Uses a velocity-based flick model rather than a fixed virtual stick, allowing natural, responsive camera panning.
- **Sticky ADS**: The Aim Down Sights button can be configured to toggle or hold.
- **HUD Layout Editor**: Pause the game and tap **Edit controls** to move, resize, adjust opacity, or hide any button. Layouts are saved in `filesDir/controls.json`.

### Physical Gamepads
- Bluetooth and USB gamepads (Xbox, DualShock/DualSense, Razer Kishi, Backbone) are supported.
- Enable or disable hardware gamepads in **Settings** to avoid phantom inputs from misreported phone sensor devices.

---

## 6. Custom Vulkan Drivers (Turnip & libadrenotools)

For devices with Qualcomm Adreno GPUs, Mesa **Turnip** drivers offer substantial performance and correctness advantages over outdated OEM vendor drivers.

1. Download a compatible Turnip driver archive (`.zip` containing `meta.json` and `libvulkan_freedreno.so`).
2. Open **Graphics Driver** in the launcher.
3. Tap **Import Driver** and choose the downloaded zip.
4. The runtime extracts the driver into private storage and loads it using `libadrenotools` in a dedicated linker namespace.

### Adreno 6xx (Snapdragon 865 class)

Measured on the Retroid Pocket Flip 2 (Adreno 650, kernel 4.19, 60 Hz), in Modern Warfare 3's Survival (Resistance). The runtime reads the chip's name from `/sys/class/kgsl/kgsl-3d0/gpu_model` before Vulkan starts (`android::perf::Adreno6xx`) and does three things on that class:

- **No UBWC.** Turnip is started with `TU_DEBUG=noubwc` (added to whatever `MW2_DRIVER_ENV` gives). With Turnip's image compression on, the whole device hard-reset within a second of a level's world first being drawn: no kgsl fault in the kernel log, the power controller reporting a shutdown the SoC asked for (`kernel.panic_on_oops` is 1 and there is no pstore, so the kernel's last words are lost). It happened at every one of a dozen level starts, with Mesa 25.1.0 and 27.0.0 alike, with `sysmem`, with draw merging off and with multi-draw off, and at none of the eighteen since with `noubwc`. Which image class trips it was not narrowed down; frame time with and without it is the same. `MW2_ADRENO6_UBWC=1` leaves the compression on, to study the reset.
- **30 frames a second, a frame every second blank.** The chip is the limit: 38 ms of GPU a frame at the console's sizes (the scene's 5,500 draws at 1024x600 are 20 ms of it, the 1280x720 passes 9, the sun's shadow maps and their resolves the rest), so 25 frames a second with the cap at 60, as a mix of two- and three-blank frames. Modern Warfare 3's FPS cap therefore starts at 30 on this class (`Prefs.fpsLimit`), which is the console's pacing at half rate ([switches.md](switches.md), `MW2_FPS_LIMIT`).
- **No sun shadows** (`MW2_SHADOWS`, unset): 31 ms a frame instead of 38, which is what lets 30 be held. `MW2_SHADOWS=1` in `.env` keeps them.

With those defaults, 246 s of play: 29.6 frames a second, 7,246 frames shown, 152 of them (2.1%) a blank late, none early. 60 is out of reach there: neither `TU_DEBUG=sysmem`, nor effects off (`fx_enable 0`, 33 frames a second uncapped), nor cheaper texture filtering brings the GPU's frame under 30 ms, and `MW2_RENDER` cannot yet give the Modern Warfare 3 campaign a smaller frame.

Those numbers are from before a pixel shader's float constants became a uniform block on this class (below). With them the opening of Modern Warfare 3's Black Tuesday, cap at 60 and no shadows, went from 44.0 frames a second to 48.3 (22.0 ms of GPU a frame to 19.9); Survival was not measured again, and its defaults are unchanged.

#### Modern Warfare 2 there: 60 frames a second

Modern Warfare 2 is lighter, and on this class it starts with the cap at 60 (`Prefs.fpsLimit`), without the sun's shadows, and with an 832x480 scene under its 1280x720 frame (`MW2_SCENE`, unset; the HUD and the menus keep the frame's size). Measured on the Flip 2 in its performance mode (the GPU's clock tops out at 670 MHz in either mode, and the GPU is what a frame waits for), Turnip v27.0.0-R2, over S.S.D.D.'s recorded flight with the player unhurt (`god`) unless said:

| | frames a second | frames on time (within 1.05 blanks) | GPU a frame |
|---|---|---|---|
| before: constants from the arena, 1024x600, no shadows | 43.1 | 31% | 21.2 ms |
| the same with the sun's shadows | 31.6 | 6% | 29.7 ms |
| pixel constants as a uniform block (below) | 54.4 | 55% | 15.4 ms |
| and the texture pages fix (`vk::textures`, the model lighting volume) | 54.9 | 57% | 15.3 ms |
| and an 832x480 scene: the defaults | 57.8 | 75% | 13.8 ms |
| a 768x448 scene instead | 57.3 | 70% | 12.7 ms |
| the defaults with the sun's shadows (`MW2_SHADOWS=1`) | 44.9 | 27% | 19.3 ms |
| the defaults, five minutes of that flight | 57.7 | 75% | 14.8 ms |
| the defaults, Terminal (a recorded walk, five minutes) | 59.3 | 92% | 13.7 ms |
| the defaults, Rust (three minutes) | 59.3 | 94% | 13.2 ms |
| the defaults, Team Player's flight, a third of its draws missing (below) | 53.9 | 45% | 11.7 ms |
| with vertex data kept from frame to frame (below): S.S.D.D., five minutes | 58.1 | 70% | 14.6 ms |
| Team Player's flight, all of it drawn | 48.9 | 42% | 12.9 ms |
| Terminal, five minutes | 59.7 | 95% | 13.8 ms |
| Rust, three minutes | 59.2 | 95% | 13.5 ms |

- **Where the time went.** A device without subgroup ballot in the fragment stage reads a draw's record a pixel at a time ([shaders.md](shaders.md#the-module-interface)), and every float constant a pixel shader read was a fetch from the arena's storage buffer: 46 to 59 buffer and texture fetches in a lit surface's pixel shader by the driver's own count (`MW2_SHADER_STATS`), of which the textures are six or eight. Read as constants of no value the frame took 16.1 ms instead of 22.4. They are a uniform block now (`MW2_PIXEL_UNIFORMS`), which the driver hands the shader without a fetch.
- **Merging is worth more than it was thought to cost.** `MW2_NO_MERGE=1` took S.S.D.D. from 40.5 frames a second to 28.4: about 210 hardware draws a frame against 2,900. Ending a run where the pixel constants change adds some hundred.
- **The scene's size.** With the constants out of the way the scene's pixels are 2.5 ms of its 9: 832x480 for 1024x600 gives back 1.5 ms, 768x448 another 1.1. The rest is its vertices, 60 to 70 fetches each, and they are what the flight's opening and Team Player wait for. A scene that changed its size with the load (it would be the renderer's to do: the title makes its targets once) would sit at its smallest exactly there, so none was built; nor an upscaling filter, whose twelve taps a pixel of the 1920x1080 screen are more fetches than the smaller scene saves [estimated, not measured].
- **Vertex data.** Turnip's storage buffers reach 128 MB, so there is no shadow of guest memory and every draw copied its vertex windows: by `simpleperf`, 48% of the ring consumer's time in Team Player was that `memcpy` (`WriteIndices` 7%, the textures' handles 13%). Worse, the copies of a Team Player frame did not fit a slot's share of the arena, and the draws after it was full were skipped: the far city and the sky, which is the black sky [MW2-ANDROID.md](MW2-ANDROID.md) had put down to the flight leaving the level. A slot now keeps the vertex data it copies while the title leaves it unwritten ([rendering.md](rendering.md#vertex-data-kept-without-the-shadow)): 4 MB a frame copied for 31, the consumer at 46% of its core for 76%, nothing skipped. With everything drawn Team Player's first half waits for the GPU, 27 ms a frame, at 35 to 44 frames a second.
- **Vertices are what is left.** A vertex shader of the world is 400 to 600 of the driver's instructions with 57 to 74 fetches (`MW2_SHADER_STATS`): 24 to 30 float constants, their blocks' places, ten dwords of vertex data, the record. The constants cannot go the pixel constants' way: 57% of the draws that join a run in Team Player and 78% in S.S.D.D. come with another vertex constant table, which as a state change would be 1,500 to 2,000 more hardware draws a frame, at 4 us each more than the fetches cost.
- **Fewer loads a vertex, and coarser models: not measured here yet.** The table's places are now one load for each block of constants and the rest of a vertex's loads fewer ([shaders.md](shaders.md#the-module-interface)), which on the Adreno 740 took a twelfth off the scene's time in Team Player's heaviest views; and `MW2_LOD_BIAS` ([rendering.md](rendering.md#detail-at-distance)) takes vertices away where there are many models. Before those, at three fixed views of Team Player's opening the scene was 19.1 ms of GPU a frame for 8,600 draws, some 1,000 hardware draws and 2.6 million indices, at 37 to 43 frames a second; a 1024x600 scene there costs 3 ms more, and models at an eighth of their distances (`MW2_LOD_BIAS=8`, 22% fewer indices) give back under 1 ms. The table above is from before both.
- **Threads.** On the 865 the ring consumer has the prime core (cpu 7) to itself, and the title's threads, the recorder, the workers and the audio the three other Cortex-A77 (4 to 6). The one thread on the Cortex-A55 was the warm-up worker, at 44% of one: it wrote the driver's whole pipeline cache, 10 MB and more, to its file after every five pipelines it found there already, 147 times in a hundred seconds. It is written now when it has grown.
- **Joined segments** (`MW2_JOIN_SEGMENTS`, from the Thor's work) measure the same here, 13.8 against 13.6 ms: Turnip draws these passes in system memory, where a pass boundary costs little.
- **The sun's shadows** are 6 ms a frame after all of the above (21.5 ms with them at the console's scene, 15.3 without): 1.7 ms drawing the two cascades' 790 draws, 2.7 ms clearing and copying the 1024x2048 map between its passes, the rest resolves and the scene's own sampling. They stay off at 60; `MW2_SHADOWS=1` with the cap at 30 holds 30.
- **Drivers.** Turnip v27.0.0 of 2026-11-17 measured the same as the bundled R2 (22.1 against 22.4 ms before the changes); faux123 1.2.10 the same until the device was lost mid-level; mainline v32 and PurpleVK T30 slower (33 and 35 frames a second against 40, T30's own GPU clock under-reporting). Qualcomm's driver (Vulkan 1.1.128) does run the renderer, through the render-pass fallback: 39.6 frames a second, with the faults [MW2-ANDROID.md](MW2-ANDROID.md) lists. `TU_DEBUG=sysmem` is what Turnip already chooses for nearly every pass (17.6 against 18.3 ms); `gmem` costs 5 ms.
- **The modes.** In the device's standard mode the baseline ran at 42.3 frames a second and 23.1 ms of GPU a frame, in its performance mode at 40.5 and 22.4 (the same flight, before the changes and before the player was kept unhurt): the GPU, at the same 670 MHz in both, was the limit, not the processor.

---

## 7. Building from Source

### Prerequisites
- **JDK 17**.
- **Android SDK** with **NDK 28.2.13676358** (`platforms;android-35`).
- CMake 3.22.1+.
- Python 3.10+.

### Local Build Steps

```sh
# 1. Recompile Xbox 360 executables to C++ on your desktop
TITLE=sp ./build.sh
TITLE=mp ./build.sh

# 2. Fetch third-party dependencies (libadrenotools)
./android/fetch_deps.sh

# 3. Compile the Campaign APK (TU0)
cd android
./gradlew assembleCampaignTu0Release

# Or compile the Multiplayer APK:
./gradlew assembleMultiplayerTu0Release

# TU6 variants append Tu6 (needs TU6 recompiled trees; CI does this):
./gradlew assembleCampaignTu6Release assembleMultiplayerTu6Release
```

The compiled APK will be located at:
- `android/app/build/outputs/apk/campaignTu0/release/*.apk`
- `android/app/build/outputs/apk/multiplayerTu0/release/*.apk`

### GitHub Actions CI
The workflow in [.github/workflows/android.yml](../.github/workflows/android.yml) automates APK builds:
1. Navigate to **Actions** → **android** → **Run workflow**.
2. Supply a direct URL to your game disc archive or executables.
3. The runner builds with `ccache` and publishes the signed APK as a build artifact.

---

## 8. Game Files & Installation Workflow

The app does not bundle game files. The player provides an Xbox 360 disc image (`.iso`, `.7z`, or `.zip` of version 1.0.557):

- **Automatic Discovery**: Placing `Call of Duty - Modern Warfare 2 (USA, Europe).iso` in `/sdcard/Download/` enables the one-tap **⚡ INSTALL & PLAY** option in the launcher.
- **Extracted Location**: The installer unpacks verified game assets into:
  `/sdcard/Android/data/com.mw2.recomp.sp/files/game/`
- **Saves Location**: Campaign and profile saves are stored in:
  `/sdcard/Android/data/com.mw2.recomp.sp/files/saves/`

---

## 9. Diagnostics & Troubleshooting

| Symptom | Cause | Solution |
| :--- | :--- | :--- |
| **"Renderer Notice: GPU lacks Vulkan 1.3..."** | Device stock driver is Vulkan 1.1 without dynamic rendering. | Check for OEM system software updates. On Adreno devices, import a Turnip driver via the Driver menu. |
| **"The runtime library would not load"** | Incompatible ABI or 32-bit device. | Ensure device supports 64-bit ARM (`arm64-v8a`). |
| **Installation fails partway** | Insufficient storage space. | Ensure at least 8 GB of free storage is available on internal storage. |
| **Log inspection** | Debugging game launch or crashes. | Inspect `/sdcard/Android/data/com.mw2.recomp.sp/files/game.log` via ADB or the in-app Log viewer. |
