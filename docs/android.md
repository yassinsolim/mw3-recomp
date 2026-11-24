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
- **RAM**: tested on 8 GB (Retroid Pocket Flip 2) and 16 GB (AYN Thor). The game's process uses about 1.8 to 2.3 GB in play (measured on the Flip 2), so 6 GB should do; 4 GB has not been tried and is likely to be closed by Android in the background or mid-game.
- **Storage**: ~7-8 GB free internal storage for game assets.

### GPU & Vulkan Driver Requirements
The renderer requires **Vulkan 1.1** with descriptor indexing: one set holds every texture, updated after it is bound and indexed non-uniformly ([What the renderer asks of a device](#what-the-renderer-asks-of-a-device)). Dynamic rendering and extended dynamic state (Vulkan 1.3, or their extensions) are used where a driver has both; without them it draws through render passes.

#### Compatibility by SoC / GPU Architecture

| SoC Family | GPU Architecture | Compatibility Status | Notes |
| **Qualcomm Snapdragon (Flagship & Mid-Range)** | Adreno 6xx, 7xx (SD 865–8 Gen 3, **SD 778G / 778G+**, 782G, 7 Gen 1/2/3, 7+ Gen 2/3) | **Supported (100% Native)** | Vulkan 1.3 supported out of the box via stock drivers or sideloadable Mesa **Turnip** drivers (`Adreno 642L` on 778G+ has complete Turnip support). Solid 60 FPS at 720p/1080p. |
| **Qualcomm Snapdragon (Entry Mid-Range)** | Adreno 619, 710 (SD 695, SD 6 Gen 1, SD 4 Gen 2) | **Supported (30–60 FPS)** | Supports Turnip and stock Vulkan 1.1/1.3. Playable with 60%–75% resolution scale. |
| **Google Tensor** | Mali-G78, Mali-G710, Mali-G715 (Pixel 6, 7, 8, 9) | **Expected to run; no phone tested** | Meets every requirement by its driver's report, every shader compiles with Arm's compiler ([Mali](#mali)). |
| **Samsung Exynos** | AMD RDNA2 / RDNA3 Xclipse (Exynos 2200, 2400) | **Supported (100%)** | Native Vulkan 1.3 driver support. |
| **MediaTek Dimensity, Samsung Exynos with Mali (Valhall and later)** | Mali-G57, G68, G77, G78, G610, G615, G710, G715, G720, Immortalis-G715 / G720 / G925 (Dimensity 700 to 9400, Helio G99, Exynos 1280 to 2100) | **Expected to run; no phone tested** | Driver r26 and later. Vulkan 1.1 drivers (r26 to r38) draw through render passes; BC textures are decoded by the runtime ([Mali](#mali)). |
| **MediaTek Helio, older Exynos and Kirin (Mali Bifrost and Midgard)** | Mali-G31, G51, G52, G71, G72, G76, T-series (Helio G80 to G95, Exynos 9611 / 9820, Kirin 980 / 990) | **Not supported** | No descriptor indexing: 256 sampled images a stage, where the renderer binds every texture of the cache in one set. The run ends at start-up and the log's `device:` line says why. |

### Mali

No phone with a Mali has run this build. The Mali testing in this repository's history (the `fix/mali-runtime-stability` and `diagnose-mali-texture-bugs` branches, a Mali-G68 MC4 and a Poco X6 Pro's Mali-G615, October 2026) was of the renderer before 29 October, when it bound a draw's textures per draw: what those testers found is still here (render passes for Vulkan 1.1 drivers, one pipeline creation at a time, the pipeline cache not reloaded from disk, BC textures decoded in software, branches translated as selections because Mali's compiler took minutes over a loop and a switch, three swapchain images), and nothing since has been seen on one. What follows was established on a PC.

**The Android Emulator cannot stand in for one.** Its guest has no GPU driver of its own: gfxstream serialises the guest's Vulkan calls and replays them on the host's driver ([Android Open Source Project, Cuttlefish GPU acceleration](https://source.android.com/docs/devices/cuttlefish/gpu): "OpenGL and Vulkan API calls are forwarded directly to the host"). The 111 reports the SDK's emulator images have filed at vulkan.gpuinfo.org name the host's chip and vendor, for example [`Goldfish GFXStream (NVIDIA GeForce RTX 3080 Laptop GPU)`, vendor NVIDIA](https://vulkan.gpuinfo.org/displayreport.php?id=52206), or llvmpipe and SwiftShader where there is none. On this PC it would be the RTX 5080's driver, limits and shader compiler under another name; and the app is `arm64-v8a` only, which an x86 system image does not load.

**Driver reports.** `tools/check_device_report.py` walks the checks the runtime makes at start-up against the report a real phone filed at [vulkan.gpuinfo.org](https://vulkan.gpuinfo.org) and says what the runtime would do with each: `python tools/check_device_report.py 53552=Mali-G57 53386=Mali-G78` (a number is a report's id there). Run on 31 Mali reports and four Adreno ones as controls (2026-11-20):

| GPU | Drivers checked | Verdict | The renderer's paths there |
|---|---|---|---|
| Mali-G57, G68 | r26, r32 (Vulkan 1.1) | runs | render passes, BC in software, no shadow of guest memory |
| Mali-G57, G68 | r38 (1.3) | runs | dynamic rendering, BC in software, no shadow |
| Mali-G57, G68 | r49, r54 (1.3) | runs | dynamic rendering, BC in software, shadow |
| Mali-G77, G78 | r32, r38 (1.1) | runs | render passes, BC in software, no shadow |
| Mali-G78 | r38 (1.3), r54 (1.4) | runs | dynamic rendering, BC in software; shadow from r44 |
| Mali-G77 | r56 (1.4, Retroid Pocket 4 Pro) | runs | dynamic rendering, BC sampled by the driver, shadow |
| Mali-G610, G710 | r32, r38 (1.1) | runs | render passes, BC in software, no shadow |
| Mali-G610, G710, G715 | r38 (1.3), r54 (1.3, 1.4) | runs | dynamic rendering, BC in software; shadow from r44 |
| Mali-G615, G720, Immortalis-G715, G720, G925 | r38, r44, r49 (1.3) | runs | dynamic rendering, BC in software; shadow from r44 |
| Mali-G52, G76 | r26, r32, r49 | **refused** | no descriptor indexing (256 sampled images a stage) |
| Adreno 650, stock and Turnip | 512.819, Mesa 26.0 | runs | a draw record a pixel (no fragment subgroup ballot), no shadow: as the Retroid Pocket Flip 2 does |
| Adreno 740, stock and Turnip | 512.819, Mesa 25.3 | runs | a draw record a subgroup, no shadow: as the AYN Thor does |

On every Mali that runs: D24S8 depth, 16:1 anisotropy, robust buffer access, 2x and 4x multisampling with a first-sample depth resolve, 32-bit indices, 128 interpolator components, half a million or more update-after-bind images and samplers, storage buffers of 256 MB (to r38) or 2 GB. Never there: multi-draw and present wait, and before r54 graphics pipeline libraries and non-seamless cube maps. Two warnings on most: the 16_16 and 16_16_16_16 unorm texture formats are sampled but not filtered linearly (a match of MW3's multiplayer uploads neither), and 32-bit float render targets do not blend (neither game draws into one).

**Shaders.** `tools/malioc_shaders.py` translates each program as the runtime would and compiles it with Arm's Mali Offline Compiler (Arm Performance Studio 2026.6, malioc 2026.5.0: the Mali driver's own compiler, r51p0 for the Mali-G57 to G78 and r56p1 from the G610 on). The programs: the 13,919 pixel shaders of MW2's fastfiles (`zone-shaders --dump`), the 309 vertex and 799 pixel shaders five MW2 levels draw with and the 228 and 376 a match of MW3's multiplayer does (`MW2_DUMP_SHADERS`, PC build), each pixel shader in the three forms a device can be given. For the Mali-G57, G68, G77, G78, G610, G710, G715 and the Immortalis-G720 and G925 **malioc refused none of them** (the fastfiles' on the G57, G78, G715 and G925). A Mali-G76 or G52 compiles the vertex shaders and the record-a-subgroup form and refuses the record-a-pixel form (`Unsupported capability (5307)`, non-uniform indexing), which is the report check's verdict from the other side. What it costs there (MW2's five levels; the Mali-G57's numbers are the G77's, and the G68's and G78's but for a few more spills):

| Pixel shader | Mali-G57: median / 95th / most load-store cycles | spills | Mali-G715 | spills | Immortalis-G925 | spills |
|---|---|---|---|---|---|---|
| a record a subgroup, constants in the arena | 40 / 74 / 113 | 516 of 799 | 38 / 63 / 92 | 449 | 38 / 63 / 93 | 391 |
| a record a subgroup, constants uniform | 26 / 48 / 78 | 335 | 26 / 42 / 58 | 213 | 26 / 41 / 58 | 164 |
| a record a pixel, constants uniform (a Mali's) | 27 / 47 / 69 | 211 | 26 / 42 / 58 | 198 | 26 / 41 / 58 | 133 |

Every shader is bound by its loads and stores, not arithmetic or texturing. Nine pixel shaders in ten use more than 32 work registers, which halves the shader core's thread occupancy on these GPUs, and a quarter still spill to the stack. No shader was refused, so the translator's output was not changed; what that register pressure costs in frames is for a phone to say. A vertex shader is 19 to 21 cycles and spills nothing up to the Mali-G715; on the Immortalis-G720 and G925 the same module is 46 and three in four are at half occupancy. Not compiled: the two small modules the runtime builds by hand (the backdrop's fragment shader and the display table's compute shader). malioc compiles and counts; it draws nothing, so it says nothing about the picture.

**What a Mali is given that another device is not** (`pipeline.cpp`, each named in the log's `device:` line):

- **A draw record a pixel.** A Mali offers the subgroup ballot to fragment shaders, in subgroups of 16, and the pixel shader that takes one record a subgroup has never run on one. The form that reads each pixel's own record has no subgroup operations, is the one the Adreno 650 draws whole levels with, and costs no more by malioc. `MW2_PIXEL_RECORDS=subgroup` is the other form, for a tester to compare.
- **Pixel constants in a uniform block** (`MW2_PIXEL_UNIFORMS`), as on an Adreno: by malioc a third fewer load and store cycles in the median pixel shader.
- **The pipeline cache is not kept between runs** unless the player asks (Settings, `MW2_PERSIST_PIPELINE_CACHE=1`): the fork's testers had drivers reload it badly.
- **Turnip is not loaded.** The bundled Turnip is only ever made the default on a Qualcomm chip (`DriverStore.installBundledDefault`), and a Turnip chosen by hand or restored from another phone's settings is refused where the chip is not an Adreno (`driver.cpp`): the log says so and the system's Vulkan is used.

**Not verified, and only a phone can:** that anything is drawn at all, and correctly: the driver's handling of the 65,536-image update-after-bind set, non-uniform indexing in both stages, the draw tables' 32-bit indices and base vertices, render passes with these on r26 to r38; frame rate and heat; the shadow of guest memory, which no Android device has had (an Adreno's storage buffers reach 128 MB); the driver's BC sampling where r54 and later offer it; whether the pipeline cache reloads on current drivers. [TESTING-MALI.md](TESTING-MALI.md) is what to ask a tester for.

#### What the renderer asks of a device

In the order the runtime checks them; `tools/check_device_report.py` is the same list, executable.

| | Required: the run ends without it | Preferred: the fallback |
|---|---|---|
| Version | Vulkan 1.1 (SPIR-V 1.3 modules) | 1.3's dynamic rendering and extended dynamic state, or their extensions: else render passes, framebuffers and static pipeline state |
| Features | descriptor indexing (sampled images update after bind, partially bound, update unused while pending), `shaderSampledImageArrayDynamicIndexing` and `...NonUniformIndexing`, `shaderDrawParameters` | `robustBufferAccess` (a warning), `textureCompressionBC` (decoded at upload by `bc_decoder.cpp` into RGBA8, RG8, R8; always on an Adreno), `occlusionQueryPrecise`, `samplerAnisotropy`, subgroup ballot in the fragment stage (else a record a pixel), `pipelineCreationCacheControl` (else the cache is one file), graphics pipeline libraries (else whole pipelines), multi-draw, non-seamless cube maps, mirror clamp to edge, present id and wait |
| Limits | 65,536 update-after-bind sampled images and 4,096 samplers a stage and a set, 68 vertex output and fragment input components, three descriptor sets, two storage buffers a stage, one dynamic uniform buffer of 4 KB, `maxStorageBufferRange` x 3 of at least 32 MB | `maxStorageBufferRange` of 1 GB for the shadow of guest memory (else vertex data is copied into the arena), 32-bit index values (else 256 draw table entries) |
| Formats | D24S8 or D32S8 as a depth attachment; RGBA8 and A2B10G10R10 drawn into, blended and sampled; RGBA8 as a storage image and blitted; each texture format of [textures.md](textures.md) sampled | 2x or 4x samples with a first-sample depth resolve (else 1x; off on Android unless asked) |
| Not used | push constants in the pipeline layout (the backdrop's own layout has 64 bytes), timeline semaphores, 64-bit integers or atomics in shaders, tessellation, geometry shaders | |

---

## 3. SoC Tuning & Performance Optimization

Modern mobile SoCs use heterogeneous CPU clusters (e.g., 1 Prime + 3 Gold + 4 Silver, or 2+2+4 on Tensor). Left to the default OS scheduler, critical guest threads can be scheduled onto low-frequency efficiency cores.

### Core Affinity (`runtime/platform.cpp`, `runtime/android/perf.cpp`)
- At startup, the runtime queries `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq` to identify the fastest CPU cores.
- The ring consumer, which parses every draw of a frame and is the one stage the title's render thread waits on, is pinned to the prime core and has it to itself. The title's main thread, the guest threads, the command recorder and the texture readers are pinned to the performance cores; a thread takes the cores of the thread that started it, so the presenter, the guest's blank and the audio worker are there too. (With the main thread on the prime core they all were, and the consumer got two thirds of it: 61.7 against 74.4 FPS in Survival on the AYN Thor at 120 Hz.)
- Efficiency cores (Cortex-A55 / A510) are reserved for background OS services and audio downmix processing.

### Dynamic Memory Management & Texture Budget
- **Device-Aware Defaults** (`ApplyMemoryDefaults`, `runtime/android/perf.cpp`): on Unified Memory Architecture the CPU and GPU share one pool, and the desktop defaults are sized for a machine with a separate one. Each default is taken from the memory this phone has, and each is set with overwrite off, so the launcher's own setting or a variable already in the environment wins:
  - **Texture budget**: a sixth of the device's memory, floor **128 MB**, capped at 1 GB and 1.5 GB at 12 GB — not the desktop "half of device-local RAM", which on a 6 GB phone is most of the phone. The whole process measured 1.8 to 2.3 GB resident in play on the 8 GB Flip 2.
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
  Whenever the activity pauses, the surface is destroyed or memory is trimmed, `vk::renderer::SaveCaches()` writes the recorded pipelines and the driver's pipeline cache; a thread of its own also writes them every 10 s while something is new, so a game swiped away or killed keeps what it compiled.
- **A first visit** (`MW2_ASYNC_PIPELINES`, on in the app): a pipeline never built is compiled by up to three threads on the performance cores while its draws are skipped; the renderer's own thread compiles none. On the AYN Thor with empty caches the surfaces of a first spawn are all there within about three seconds and no frame is held (the longest 68 to 83 ms in Seatown, Bakaara, Afghan and Survival); a second visit compiles nothing it drew before ([rendering.md](rendering.md)).
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
- **The cap starts at the screen's rate.** Before the texel forms this chip drew Modern Warfare 3 at 25 frames a second with the cap at 60 (38 ms of GPU a frame at the console's sizes), so the cap used to start at 30 here. With them Survival runs at 58 and Black Tuesday at 56 under a cap of 60 (the table below), so both titles now start at the display's fastest rate like any other phone, 60 on the Flip 2 (`Prefs.fpsLimit`); FPS LIMIT 30 in the launcher still gives the console's pacing at half rate ([switches.md](switches.md), `MW2_FPS_LIMIT`).
- **No sun shadows** (`MW2_SHADOWS`, unset): about 6 ms of GPU a frame less, which is what lets 60 be approached. `MW2_SHADOWS=1` in `.env` keeps them, best with FPS LIMIT 30.
- **An 832x480 scene** in both titles (`MW2_SCENE`, unset), under the 1280x720 frame the HUD and the menus keep. Modern Warfare 2 had it first (below); Modern Warfare 3 takes the scene through the same function since it was found in its executables, and with the cap at 60 it is what holds it. Five minutes each on the Flip 2, `.dev` builds, caches full (the second run of each), the console's scene against this one:

  | Modern Warfare 3, cap 60 | frames a second | frames on time | stutters (frames half as long again as due) |
  |---|---|---|---|
  | Survival (Resistance), 1024x600 | 55.8 | 36.7% | 251 |
  | Survival, 832x480 | 59.9 | 76.8% | 42 |
  | Black Tuesday, 1024x600 | 58.9 | 79.2% | 152 |
  | Black Tuesday, 832x480 | 59.6 | 79.7% | 95 |

  Survival's five minutes are a first minute of play and, once the player has fallen, the same map behind the result; the Thor's below are the same. In that first minute 896x528 gave 59.4 with 51 stutters and 768x448 59.6 with 47 against 832x480's 59.7 with 35 and the console's 57.5 with 74. `MW2_SCENE=1024x600` keeps the console's.

Measured with the old 30 cap, 246 s of play: 29.6 frames a second, 7,246 frames shown, 152 of them (2.1%) a blank late, none early. That was before the texel forms; with them a cap of 60 is nearly held (the table below).

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
| after a vertex's loads were made fewer (below), 2026-11-20: S.S.D.D., five minutes | 59.9 | 91.5% | 13.7 ms |
| Team Player's flight | 52.5 | 57% | 16.3 ms |
| Terminal, 170 seconds | 59.3 | 95.5% | 13.4 ms |
| Rust, three minutes | 59.3 | 95.7% | 13.2 ms |
| with vertex data copied in host byte order (`MW2_HOST_ORDER`, below): S.S.D.D., five minutes | 59.8 | 92.0% | 13.7 ms |
| Team Player's flight | 52.6 | 56% | 16.3 ms |
| Terminal, 170 seconds | 59.5 | 94.8% | 13.3 ms |
| Rust, three minutes | 59.4 | 95.6% | 13.0 ms |
| with the pixel shaders' record as texels (`MW2_PIXEL_TEXELS`, below), 2026-11-21: S.S.D.D., five minutes | 59.9 | 93.4% | 10.1 ms |
| Team Player's flight | 59.5 | 87.9% | 11.4 ms |
| Terminal, five minutes | 60.0 | 97.5% | 9.4 ms |
| Rust, three minutes | 59.9 | 95.6% | 8.5 ms |

- **Where the time went.** A device without subgroup ballot in the fragment stage reads a draw's record a pixel at a time ([shaders.md](shaders.md#the-module-interface)), and every float constant a pixel shader read was a fetch from the arena's storage buffer: 46 to 59 buffer and texture fetches in a lit surface's pixel shader by the driver's own count (`MW2_SHADER_STATS`), of which the textures are six or eight. Read as constants of no value the frame took 16.1 ms instead of 22.4. They are a uniform block now (`MW2_PIXEL_UNIFORMS`), which the driver hands the shader without a fetch.
- **Merging is worth more than it was thought to cost.** `MW2_NO_MERGE=1` took S.S.D.D. from 40.5 frames a second to 28.4: about 210 hardware draws a frame against 2,900. Ending a run where the pixel constants change adds some hundred.
- **The scene's size.** With the constants out of the way the scene's pixels are 2.5 ms of its 9: 832x480 for 1024x600 gives back 1.5 ms, 768x448 another 1.1. The rest is its vertices, 60 to 70 fetches each, and they are what the flight's opening and Team Player wait for. A scene that changed its size with the load (it would be the renderer's to do: the title makes its targets once) would sit at its smallest exactly there, so none was built; nor an upscaling filter, whose twelve taps a pixel of the 1920x1080 screen are more fetches than the smaller scene saves [estimated, not measured].
- **Vertex data.** Turnip's storage buffers reach 128 MB, so there is no shadow of guest memory and every draw copied its vertex windows: by `simpleperf`, 48% of the ring consumer's time in Team Player was that `memcpy` (`WriteIndices` 7%, the textures' handles 13%). Worse, the copies of a Team Player frame did not fit a slot's share of the arena, and the draws after it was full were skipped: the far city and the sky, which is the black sky [MW2-ANDROID.md](MW2-ANDROID.md) had put down to the flight leaving the level. A slot now keeps the vertex data it copies while the title leaves it unwritten ([rendering.md](rendering.md#vertex-data-kept-without-the-shadow)): 4 MB a frame copied for 31, the consumer at 46% of its core for 76%, nothing skipped. With everything drawn Team Player's first half waits for the GPU, 27 ms a frame, at 35 to 44 frames a second.
- **Vertices are what is left.** A vertex shader of the world is 400 to 600 of the driver's instructions with 57 to 74 fetches (`MW2_SHADER_STATS`): 24 to 30 float constants, their blocks' places, ten dwords of vertex data, the record. The constants cannot go the pixel constants' way: 57% of the draws that join a run in Team Player and 78% in S.S.D.D. come with another vertex constant table, which as a state change would be 1,500 to 2,000 more hardware draws a frame, at 4 us each more than the fetches cost.
- **Fewer loads a vertex.** The table's places are now one load for each block of constants and the rest of a vertex's loads fewer ([shaders.md](shaders.md#the-module-interface)): 27 loads a vertex for 66. With that the table's last four rows: S.S.D.D. holds 60 for five minutes, and Team Player's flight went from 48.9 to 52.5 frames a second.
- **What Team Player still waits for.** At three fixed views of its opening the frame is 8,500 draws in some 1,000 hardware draws, 2.65 million indices, and 16.5 to 18 ms of GPU for the scene alone, at 41 to 44 frames a second. The scene at 512x300, 38% of the pixels, is 13.5 ms and 52.5 frames a second, so the pixels are 5 to 6 ms of it and the vertices 11 to 12, which no size of scene gives back; a 1024x600 scene costs 2 to 3 ms. Models at an eighth of their distances (`MW2_LOD_BIAS=8`, 22% fewer indices) give back under 1 ms, so detail at distance stays the console's here too. None of what follows moved those views by more than one run differs from the next (41 to 47 frames a second, the scene 15.4 to 18.0 ms):
  - Turnip's choice of where a pass is drawn: `TU_AUTOTUNE_ALGO=prefer_sysmem` 43.9, `profiled` 41.6, `TU_AUTOTUNE_FLAGS=tune_small` 44.5, `TU_DEBUG=sysmem` 42.9, `nobin` 41.9; `gmem` 32.7. The passes are in system memory already, where a vertex is shaded once.
  - `IR3_SHADER_DEBUG=noearlypreamble`: 42.4.
  - The low-resolution depth test (LRZ). `TU_DEBUG=perf` says Turnip gives it up at "Depth write + ALWAYS/NOT_EQUAL", twice a frame: the pair of clear rectangles the title opens the scene with, which this renderer draws. Answered with a clear of the depth image and the rectangles drawn for their colour alone, the message goes and nothing else changes (the scene 15.4 and 16.6 ms against 16.2 in Team Player, 10.45 against 10.5 in S.S.D.D.'s opening), as `TU_DEBUG=nolrz` changes nothing either: the title's own stencil tests and blended depth writes end LRZ's writes early in the pass ("Stencil may kill fragments", "Depth write + blending"). The rectangles are drawn as before.
  - Vertex buffers instead of loads from the arena (the `vertex-input` branch's builds): the scene 17.3 to 19.4 ms and the ring consumer at 65 to 80% of a core for 44.
  - Barriers: counted, 32 a frame (122,318 in 3,814 frames), every one a change of an image's layout between an attachment and a copy's source or destination, outside any pass. None is empty, so there is none to skip.
- **Vertex data in host byte order** ([rendering.md](rendering.md#vertex-data-in-host-byte-order)) is the one thing that moved Team Player's views: 42.9 and 42.5 frames a second to 44.9 and 46.2, the scene 17.0 ms to 15.8. It is the default wherever there is no shadow of guest memory, so there is nothing for this class to set. The flight as a whole is as it was, and 60 is not held there: its first half runs at 37 to 50. Modern Warfare 3 with it, at its limit of 30: Survival on Resistance from the menus 30.0 frames a second in play, Black Tuesday from Mission Select 29.7.
- **What a vertex shader costs there, and the arena as texels.** Turnip will say what it compiled a shader to (`MW2_SHADER_IR`, [switches.md](switches.md)). Two vertex shaders run three vertices in five of Team Player: the depth pass's, 207 instructions with 45 fetches in 21 registers, and the lit world's, 348 with 61 in 32. The depth pass's is its own binning variant, a position and nothing else, and the lit world's binning variant is 229 instructions with 48 fetches: the position alone is most of the work, and the outputs a pixel shader does not read are dropped by the driver already. The cost is how the arena is read: ir3 loads a storage buffer one dword a fetch on a 6xx, three instructions each, a vec4 four of them, and the loads waited on each other four deep. With the scene's every primitive put outside the clip volume (a test build), at 587 MHz, the scene's 9 passes are 3.9 ms with vertex shaders that compute nothing and 10.3 ms with the real ones reading only their vec4 as texels: some 6.4 ms of vertex shading, 4 of primitives, and the rest of the scene's 15 to 16 its pixels. So on this class under Turnip the vertex shaders read the arena as texels ([shaders.md](shaders.md#the-module-interface), `MW2_VERTEX_TEXELS`): the depth pass's shader is 99 instructions with 11 fetches in 10 registers, the lit world's 204 with 16 in 16, and the same culled scene is 7.5 ms, 3.5 of it vertex shading. Measured against `MW2_VERTEX_TEXELS=0` in the same build, the cache warm for each:

  | | without | with |
  |---|---|---|
  | Team Player's three heavy views, frames a second | 43.8, 44.2, 45.9 | 50.0, 51.5, 52.6, 53.5 |
  | their scene, GPU ms a frame | 15.7, 16.5, 16.8 | 12.5, 12.9, 13.2, 14.1 |
  | Team Player's flight: frames a second | 53.0, 52.9 | 56.6, 56.2 |
  | frames on time | 54.5%, 55.8% | 69.3%, 69.4% |
  | its first half | 45.5, 45.2 | 51.7, 50.1 |
  | its worst ten seconds | 41.1, 39.0 | 48.8, 46.8 |
  | GPU ms a frame | 16.1, 16.1 | 14.6, 14.8 |
  | S.S.D.D., four fixed views (cap 60): GPU ms a frame | 12.3, 12.5 | 11.0, 11.0 |

  With it, S.S.D.D. flown for five minutes is 59.9 frames a second with 93.7% of the frames on time (59.8 and 92.0% the day before), Terminal 59.8 and 96.0% (59.5 and 94.8%), Rust 59.5 and 95.8% (59.4 and 95.6%). The picture is the same: at four fixed views of S.S.D.D. 64.3 to 99.7% of the pixels are identical to the bit with and without, 65.3 to 99.7% between two runs without, and what differs is what moves. Team Player is still not a locked 60: its first half is 50, and what is left of its scene is some 3 ms of vertex shading, 3.5 of primitives and 6 of pixels.

  Modern Warfare 3 with and without, in the same build with the cache warm for each (`.dev`, levels started from the menus, Mission Select opened with `mis_cheat 1`). At its limit of 30 the limit is what shows; with the limit at 60 the chip is:

  | | without | with |
  |---|---|---|
  | Survival on Resistance, limit 30: frames a second | 30.0 | 30.0 |
  | frames within two display periods | 90.9% | 89.9% |
  | GPU ms a frame | 25.3 | 23.8 |
  | the same at its first spawn, limit 60: frames a second | 42.3 | 46.4 |
  | GPU ms a frame (the scene) | 23.5 (16.6) | 21.6 (15.3) |
  | Black Tuesday's opening, limit 30: frames a second | 29.6 | 29.8 |
  | frames within two display periods | 89.8% | 91.1% |
  | GPU ms a frame | 25.1 | 23.1 |
  | the same, limit 60: frames a second | 42.1 | 45.3 |
  | GPU ms a frame (the scene) | 22.4 (17.7) | 20.7 (16.1) |
  | Seatown with six bots, limit 30: frames a second | 30.0 | 30.0 |
  | frames within two display periods | 89.5% | 88.3% |
  | GPU ms a frame, from the same spawn | 20.0 | 18.3 |

  No run logged a renderer error, and the draws skipped are of the same kinds either way. The picture is the same: in the menus 53 to 100% of the pixels are identical to the bit with and without (99 to 100% in the campaign's at 30), 56 to 86% between two runs of one way, whose background drifts; at the first spawn of each level the mean difference of a pixel's channels is 1.2 to 5.1 of 255 with and without at 30, and 3.3 or more between two runs of one way: fire, smoke, the weapon's sway and the bots.
- **What a pixel shader costs there, and its record as texels** (2026-11-21, `MW2_PIXEL_TEXELS`, [shaders.md](shaders.md#the-module-interface)). The same reading of Turnip's assembly for the other stage: a lit surface's pixel shader in Team Player was 470 to 850 instructions with 26 to 44 fetches of single dwords of the storage buffer for its 7 to 13 textures -- the alpha test's four words, and for each texture its word of the record and two dwords of its fetch constant -- plus a `getsize`, two reciprocals and an `exp2` a texture. A test build that read the fetch constants and the sizes as nothing held 59 frames a second at the heavy views where there were 50. The record now has a texel for each texture with all of that in it, and a pixel fetches that and the alpha test's texel. In one build, each way with its pipelines cached:

  | | without | with |
  |---|---|---|
  | Team Player's two steady heavy views, frames a second | 48.6, 51.4; 51.2, 47.8 | 59.5, 59.1; 59.8, 59.5 |
  | GPU ms a frame there (the scene) | 18.3 (13.0), 19.1 (13.9) | 15.2 (11.2), 15.4 (11.3) |
  | the 1280x720 passes after the scene, GPU ms | 2.94 | 1.73 |
  | Team Player's flight: frames a second, on time (without: the build before) | 56.6, 69.3% | 59.5, 87.9% |
  | its first half | 51.2, 49.6 | 59.0 |
  | its worst ten seconds | 48.8, 46.8 | 57.3 |

  The table above has S.S.D.D., Terminal and Rust with it. The picture: four fixed views of S.S.D.D. against that morning's captures without, 66.9 to 92.6% of the pixels the same to the bit, 65.6 to 94.6% between two runs without, the differences in the same places (what moves). Without the switch the modules are the bytes they were: the build found all 1,133 pipelines of the cache made before the change. Counted on the way (`MW2_COUNT_PRIMITIVES`): a frame there is 8,200 draws and 860,000 triangles, 43% of them drawn for depth alone, 91% with back faces culled and 9% with none, as the title sets it; 537 draws are of one or two triangles and hold 927 triangles, so small draws are not where the triangles are. 525 of the 7,100 draws that join a run come with other textures or another alpha test, which is what handing the record to the pixel shader as a uniform block would cost in hardware draws; it was not built. The vertex shader without the window transform (a test build that left it out of every shader that reads a constant): the scene 11.2 ms against 11.3, inside what one run differs from the next, so there is no second form of the vertex shaders.

  Modern Warfare 3 with and without (`MW2_PIXEL_TEXELS=0`), one `.dev` build, the cache warm for each, the levels started from the menus as above:

  | | without | with |
  |---|---|---|
  | Survival on Resistance, limit 30: frames a second | 30.0 | 30.0 |
  | frames within two display periods | 91.8% | 91.5% |
  | GPU ms a frame (the scene) | 24.2 (16.9) | 18.2 (12.1) |
  | the same, limit 60: frames a second | 46.1 | 58.2 |
  | GPU ms a frame (the scene) | 21.6 (15.2) | 16.6 (11.2) |
  | Black Tuesday's opening, limit 30: frames a second | 29.8 | 30.0 |
  | frames within two display periods | 90.4% | 91.2% |
  | GPU ms a frame (the scene) | 22.9 (17.8) | 15.9 (11.6) |
  | the same, limit 60: frames a second | 46.3 | 55.8 |
  | GPU ms a frame (the scene) | 20.1 (15.5) | 15.0 (11.2) |

  No renderer error either way, the draws skipped of the same kinds. The picture: the menus' mean difference of a pixel's channels is 0.03 to 0.2 of 255 with and without, 0.05 to 0.6 between two runs of one way; in play (the weapon sways, the fires burn) 5.4 to 15 in Black Tuesday with and without against 4.0 to 17 and 4.8 to 22 between two runs of one way, and Survival's first spawn likewise.
- **Threads.** On the 865 the ring consumer has the prime core (cpu 7) to itself, and the title's threads, the recorder, the workers and the audio the three other Cortex-A77 (4 to 6). The one thread on the Cortex-A55 was the warm-up worker, at 44% of one: it wrote the driver's whole pipeline cache, 10 MB and more, to its file after every five pipelines it found there already, 147 times in a hundred seconds. It is written now when it has grown.
- **Joined segments** (`MW2_JOIN_SEGMENTS`, from the Thor's work) measure the same here, 13.8 against 13.6 ms: Turnip draws these passes in system memory, where a pass boundary costs little.
- **The sun's shadows** are 6 ms a frame after all of the above (21.5 ms with them at the console's scene, 15.3 without): 1.7 ms drawing the two cascades' 790 draws, 2.7 ms clearing and copying the 1024x2048 map between its passes, the rest resolves and the scene's own sampling. They stay off at 60; `MW2_SHADOWS=1` with the cap at 30 holds 30.
- **Drivers.** Turnip v27.0.0 of 2026-11-17 measured the same as the bundled R2 (22.1 against 22.4 ms before the changes); faux123 1.2.10 the same until the device was lost mid-level; mainline v32 and PurpleVK T30 slower (33 and 35 frames a second against 40, T30's own GPU clock under-reporting). Qualcomm's driver (Vulkan 1.1.128) does run the renderer, through the render-pass fallback: 39.6 frames a second, with the faults [MW2-ANDROID.md](MW2-ANDROID.md) lists. `TU_DEBUG=sysmem` is what Turnip already chooses for nearly every pass (17.6 against 18.3 ms); `gmem` costs 5 ms. At Team Player's three views later: TurnipZ 0.4.0, a fork for this chip, the same as R2 (44.4 frames a second, the scene 16.6 ms); K11MCH1's Turnip 24.1.0 R18 for a6xx 32.1, the scene 25 ms. The bundled R2 stays.
- **Image compression stays off, and is not what is missing.** The reset is put down to compressed images that are also copied from (`VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, nckstwrt/NetherSX2-Turnip issue 70). The bundled Turnip takes `VK_EXT_image_compression_control`, so a test build left the compression on (`MW2_ADRENO6_UBWC=1`) and asked for it off image by image, all but one class at a time. At Team Player's three views, against 45.9 frames a second and a scene of 16.0 ms without compression in the same sitting: the textures alone compressed 41.6 and 17.7 ms, the colour targets 44.0 and 16.6, the depth targets 45.1 and 16.4, the images resolves are copied into 44.2 and 16.7, all four 45.3 and 16.3, and every image (the switch alone) 42.8 and 17.7. Nothing gained, which is the vertices again; and no reset in those six starts of a Modern Warfare 2 level, where Modern Warfare 3's Survival had one at every start. `noubwc` stays for both games, and the test build's switch was not kept.
- **The modes and the clocks.** In the device's standard mode the baseline ran at 42.3 frames a second and 23.1 ms of GPU a frame, in its performance mode at 40.5 and 22.4 (the same flight, before the changes and before the player was kept unhurt): the GPU, at the same 670 MHz in both, was the limit, not the processor. `/sys/class/kgsl/kgsl-3d0/gpuclk` read every four seconds of play is 670 MHz, the device's `max_gpuclk`, in seven readings of eight where the frame is late (Team Player) and four of five where the cap is held (S.S.D.D.), 587 in the rest, with the chip 83 to 89% busy; the fast cores reach their 2,419 and 2,842 MHz. There is nothing above that for a performance hint to ask for. In the five-minute runs the GPU's zone reached 63 C, the processor's 72, the skin's 52.

### Adreno 7xx (Snapdragon 8 Gen 2 class)

Measured on the AYN Thor (Adreno 740, 120 Hz, the bundled Turnip v27.0.0-R2), 2026-11-21, `.dev` builds with the limit at 120.

- **The arena as texels is the default there too** (`MW2_VERTEX_TEXELS`, `MW2_PIXEL_TEXELS`, [shaders.md](shaders.md#the-module-interface)): on every Adreno under Turnip, where it was the 6xx alone. Turnip's compiler has a load of several dwords from the 7xx on, and the texels are faster all the same. GPU time a frame and frames a second, two runs each way after one to fill the pipeline cache, switched in one build:

  | | neither | vertex | pixel | both |
  |---|---|---|---|---|
  | S.S.D.D.'s opening | 9.08 ms, 99.8 | 7.88 ms, 104.3 | 8.69 ms, 101.6 | 7.43 ms, 106.1 |
  | S.S.D.D., four fixed views | 6.49 ms, 117.3 | 5.89 ms, 117.6 | | 5.63 ms, 118.1 |
  | Team Player's three heavy views | 13.16 ms, 70.2 | 11.19 ms, 73.2 | 12.22 ms, 73.5 | 10.35 ms, 73.5 |
  | Rust (at the limit) | 6.48 ms, 119.0 | 5.99 ms, 118.8 | 6.36 ms, 119.0 | 5.54 ms, 119.0 |
  | Modern Warfare 3, Survival on Resistance | 10.87 ms, 86.7 | 9.17 ms, 102.6 | 10.23 ms, 91.0 | 8.52 ms, 107.8 |

  Both together are 13 to 22% less GPU time everywhere. Team Player's views gain little in frames a second because the ring consumer is their limit there (87 to 95% of its core). The picture is the same: at S.S.D.D.'s four views 68.2 to 99.1% of the pixels are identical to the bit with both and with neither, 68.3 to 99.1% between two runs with neither; Modern Warfare 3's menus 96.9 to 100%; in play what differs is what moves. No run logged a renderer error.
- **No draw is skipped for want of arena** ([rendering.md](rendering.md#a-frame-larger-than-its-slots-share)). Team Player's flight skipped 34,197 draws as `frame arena full` on this device, and 8,486 on the Retroid Pocket Flip 2: frames with the far buildings and the sky missing, at the level's start, after each checkpoint loaded again and whenever a slot's kept vertex data had filled its share. With a draw made in the next slot when its own is full, and the vertex data kept by block, the same flight skips none on either device (1 or 2 draws made in the next slot, no slot letting its copies go, 61 to 72 MB kept at most), nor do five minutes of S.S.D.D. or Modern Warfare 3's Survival.
- **Modern Warfare 3 without the sun's shadows above 60** (`MW2_SHADOWS`, unset; the app's Sun shadows setting gives them back). Under the limit of 120 the ring consumer is what a frame waits for (98% of its core at work, 2% of that waiting for the chip), and the shadow maps' passes are three in ten of the draws it parses. Five minutes each, caches full:

  | Modern Warfare 3, limit 120 | frames a second | frames on time | stutters |
  |---|---|---|---|
  | Survival (Resistance), with shadows | 106.0 | 36.6% | 1,291 |
  | Survival, without | 118.2 | 86.7% | 122 |
  | Black Tuesday, with | 114.9 | 78.1% | 490 |
  | Black Tuesday, without | 118.4 | 89.5% | 183 |

  A smaller scene does nothing for it with the shadows on (832x480: 106.2 frames a second in Survival's first minute against 107.1) and little without them (116.2 against 114.5), so the scene stays the console's there. Modern Warfare 2 keeps its shadows: S.S.D.D.'s flight is 115.9 with them. Team Player's flight is 102.1 with and 111.6 without, which is there to take if that level is to hold more.
- **A screen the system holds at 60.** The Thor's display setting (`peak_refresh_rate` and `min_refresh_rate`, which its dual-screen assistant writes, and wrote 60 into after a restart) outranks an app's request for a mode, so the limit of 120 it starts with ran on a 60 Hz screen: 125 frames drawn a second by the title's limiter to show 60 of them. A limit above the display's rate is paced by the display now, one frame a blank ([switches.md](switches.md), `MW2_FPS_LIMIT`), and the app says once at the start of a run that the screen is set to 60 Hz and where to change it.

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

- **Automatic Discovery**: Placing `Call of Duty - Modern Warfare 2 (USA, Europe).iso` in a `Download` folder, the device's or a memory card's, enables the one-tap **⚡ INSTALL & PLAY** option in the launcher.
- **Game folder**: `/sdcard/Android/data/com.mw2.recomp.sp/files/game/` holds the executables and the picture (11 MB) of a game played from its image, or every file of one that was copied in; mods, camouflage packs and an update's files are there either way.
- **Saves Location**: Campaign and profile saves are stored in:
  `/sdcard/Android/data/com.mw2.recomp.sp/files/saves/`
- **Start-screen artwork** (`DiscArt.kt`): every disc has `nxeart` at its root, the package the console's dashboard takes the game's backdrop from. The installer copies it with the game files; the launcher takes `nxebg.jpg` (1920x1080) out of it once, keeps it in the app's private folder (`files/art/`), and draws it behind the menu under a scrim (`BackdropView.kt`), cropped about its middle on a screen that is not 16:9. It is never in the APK or the repository: without the game files, or with a package it cannot read, the launcher keeps its own haze. The home-screen icon is always the project's emblem.

### The game files

Two ways, chosen on the install screen (`InstallActivity.kt`) and changed later from **Settings → Files → Change where the game is read from**:

- **Play from the disc image** (the default). The runtime lays the image under the game folder and reads the game's files out of it where it lies ([runtime.md](runtime.md#files)); the installer checks the executables and copies only them and `nxeart`. The image has to stay where it is. The app keeps it as a path, or, for one chosen with the Files app, as the document's address with the picker's permission kept (`DiscImage.kt`): the game's process opens the document and hands the runtime the descriptor, so an image on a memory card needs no all-files access. When the image is gone (deleted, moved, the card out), the launcher says so and offers the install screen, and a run started anyway ends with the same sentence.
- **Copy the game files into the app**, about 7 GB, from an image or a folder of a disc's files; the source is not needed afterwards. Going from copied to the image, the app offers to delete the copies (the files of the image's names and sizes; anything else in the folder stays).

Writable data is the app's in both: saves, settings, the shader and pipeline caches, `camos/`, `mods/`, `dlc/` ([dlc.md](dlc.md)).

Measured on a Retroid Pocket Flip 2 (Snapdragon 865, UFS; the memory card exFAT at 59 MB/s sequential), from the level's console command to the first frame of play, with the file cache pushed out before each run by reading 16 GB of other files (8 GB of memory); two or three runs each:

| | image, device storage | image, memory card | copied files |
|---|---|---|---|
| Modern Warfare 2, S.S.D.D. (51 MB fastfile) | 3.6 s | 3.8 to 3.9 s | 3.7 to 3.8 s |
| Modern Warfare 2, The Gulag (87 MB) | 6.2 s | 6.8 to 7.0 s | 6.2 to 6.3 s |
| Modern Warfare 2 multiplayer, Terminal | 5.9 to 6.5 s | | |
| Modern Warfare 3, Survival on Resistance | 13.2 to 13.5 s | | |
| Call of Duty 4, F.N.G. (high mips from the image's `highmip`) | 5.1 s | | |

Late frames in the half minute after the load: none from the device's storage or from copied files, none or one from the card. The card's image chosen with the Files app (read through the descriptor) loads as the one named by its path does (3.9 and 6.9 s). Copying Modern Warfare 2's files took about half a minute; setting up from the image takes under a second. On the AYN Thor, one uncontrolled run of S.S.D.D. from the image loaded in 4.0 s; the comparison was not repeated there.

---

## 9. Diagnostics & Troubleshooting

| Symptom | Cause | Solution |
| :--- | :--- | :--- |
| **"Renderer Notice: GPU lacks Vulkan 1.3..."** | Device stock driver is Vulkan 1.1 without dynamic rendering. | Check for OEM system software updates. On Adreno devices, import a Turnip driver via the Driver menu. |
| **"The runtime library would not load"** | Incompatible ABI or 32-bit device. | Ensure device supports 64-bit ARM (`arm64-v8a`). |
| **Installation fails partway** | Insufficient storage space. | Ensure at least 8 GB of free storage is available on internal storage. |
| **"The game could not start: the game code is not on this device"** | The app is published without the game's code, which is built from the player's disc on a PC ([game-library.md](game-library.md)). The game files alone (Install & Play) are not enough. | Run `setup\setup.cmd` on a PC with the device connected over USB. The log says `app: the run was not started: ...` and `game code: ... is not installed`. |
| **Log inspection** | Debugging game launch or crashes. | The in-app Log viewer's **Share**, or `/sdcard/Android/data/com.mw2.recomp.sp/files/game.log` via ADB. |

### What the log share holds

**Share** on the Log screen sends one text that is enough for a bug report
(`LogSummary.kt`, `DeviceInfo.kt`), 200,000 characters at most:

- `===== report =====`: the app and its version, the phone, its chip, its
  memory, the GPU with its driver's version (asked of OpenGL ES in the
  launcher, so a run that dies opening the Vulkan driver has still said it),
  and whether the game code package is installed. The same lines open
  `game.log` and `launcher.log`.
- How the last six of the app's processes ended, as Android recorded it
  (`ApplicationExitInfo`, Android 11 on): killed for memory, crashed, stopped
  by the user, exited by itself, with the memory each held. A process the
  system killed writes nothing itself, so this is the only account of it. For
  a native crash (Android 12 on) the signal and the crashing thread's
  backtrace are read out of the system's tombstone.
- `game.log` and `launcher.log` (their ends), the last crash files, and what
  each process was in the middle of (`boot-trail-*.txt`).

`game.log` has a `step:` line before each part of start-up that can end the
process without a word: `game-code`, `start`, `image`, `online`,
`guest-memory`, `kernel`, `gpu`, `vulkan-library`, `surface`,
`vulkan-instance`, `vulkan-surface`, `vulkan-device`, `swapchain`,
`pipeline-layout`, `textures`, `arena`, `guest`. Each line is written to the
file as it is logged, so the last one names what the process was doing when
it died. A fault's report quotes it (`last step:`), covers `abort()` and a
refused system call as well as a bad access, and is in the file before the
process ends; the process then dies of the signal itself, so Android records
a native crash with a tombstone. `MW2_DEBUG_CRASH_AT=<step>`
([switches.md](switches.md)) faults at a step on purpose, to check all of
this on a device.
