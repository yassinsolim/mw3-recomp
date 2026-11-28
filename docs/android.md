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
| **Qualcomm Snapdragon (older mid-range)** | Adreno 610-619 (SD 662 / 665 / 680, 675, 710 / 712, 720G / 730 / 732G, 750G) | **Expected to run with Turnip; untested** | Their stock drivers of 2019-21 (512.502, 512.530) have no descriptor indexing and are refused; Turnip has it on every Adreno 6xx, and the app now gives these phones its bundled Turnip on Android 10 and 11 too ([Older Snapdragons](#older-snapdragons-adreno-610-619-android-10-and-11)). 20-30 FPS on an SD 7xx, by benchmark ratios to the Snapdragon 865 ([older-hardware.md](drafts/older-hardware.md#6-what-frame-rate-to-promise)). |
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
- **The pipeline cache is kept between runs from driver r38 on**, and not under older drivers unless the player asks (Settings, `MW2_PERSIST_PIPELINE_CACHE=1`; `=0` refuses it under any). It was off on every Mali from October 2026 on the word of "known driver deadlocks", but the change that turned it off (6089570, a hang at 97% of the boot compile on a Mali-G68 MC4, no driver version or log kept) also took away the lock every creation waited on, so which of the two hung was never shown; the cache within a run came back the same day and works on Malis, and no log has since shown a reloaded cache failing on one. Arm's own advice is to save and reload it, and a Mali's `pipelineCacheUUID` changes with every driver release (vulkan.gpuinfo.org). Before the driver sees a blob its header has to name this device, driver version and UUID, and so does the driver's own `VkPipelineCacheHeaderVersionOne` at the start of it (`pipeline_cache_rules.h`, tests/pipeline_cache_test.cpp). Pipelines are still created one at a time on a Mali under every driver: with the lock gone, the same Mali-G68 MC4 hung in its compiler with two under way (11bcf04), and a Mali-G52/G57 under r49p1 has been seen to crash the same way elsewhere.
- **Compile all shaders at boot** shows a card over the black screen with the count, and says whether the wait comes back at the next start (`BootCompile.kt`). On a Galaxy A35 (Mali-G68, r38p1) it is 1,064 pipelines at 60 to 500 ms each: three to five minutes the first time.
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

### What a device is given, and what it is measured for

The project owns two devices, an AYN Thor (Adreno 740, 120 Hz) and a Retroid Pocket Flip 2 (Adreno 650, 60 Hz), and until 26 November 2026 what a run started with was what those two were measured to want, chosen by the chip's name. Players of other chips then reported Advanced Warfare at 20 to 30 frames a second with choppy sound on a Snapdragon 8s Gen 3 (Adreno 735) whose Call of Duty 4 ran at 120, 50 on a Snapdragon 8 Gen 1 (Adreno 730), and a black screen on a device nobody named. None of those devices is here; what follows was built and measured on the two that are, with a load put on them to stand in for a weaker one.

**What each device was given before**, and where it came from:

| | Adreno 650 (the Flip 2) | Adreno 710, 730, 735, 750 | Adreno 740 (the Thor) | not an Adreno, or nothing said |
|---|---|---|---|---|
| FPS limit (`Prefs.displayLimit`) | the screen's fastest of 30, 45, 60, 120 | the same: 120 on most of their screens | 120 | the same |
| Driver (`DriverStore`) | the bundled Turnip on any Qualcomm chip, Android 12 on | the same, whether or not that Turnip knows the chip | the bundled Turnip | the system's |
| Sun shadows, unset | off: an Adreno 6xx | off above a limit of 60, **on** at 60 and under | off at 120 | as the 7xx |
| The scene | 832x480 in Modern Warfare 2 and 3 and Advanced Warfare | the console's 1024x600 | 1024x600 | 1024x600 |
| The scene drawn smaller while the chip is late | yes | **no** | no | no |
| Turnip's image compression | off (it resets the device) | on | on | |
| Texture cache, upload arena, vertex shadow, shaders compiled at the start | by the device's memory and cores (`perf.cpp`): a sixth of memory up to 1 GB (1.5 GB from 12 GB), 192 to 384 MB, the shadow above 6 GB, 15 to 40 pipelines | the same | the same | the same |
| Resolution 70%, no multisampling | every device | | | |

So an Adreno 730 or 735 on a 120 Hz screen was held to 120 with the console's scene at its whole size whatever it drew, and at 30 frames a second showed them at uneven steps of the game's time; on a 60 Hz screen it had the sun's shadows as well, which the 740 has the room for. Nothing was ever given back.

**What a device is given now** (`runtime/adaptive.h`, `MW2_ADAPTIVE`, on under Android) is decided by two-second windows of play -- world frames one after another; a menu, a loading screen or a movie says nothing -- from the frames a second, the graphics chip's time a frame by its own clock reckoned at its fastest clock (`gpu_time.h`), and the renderer's thread's time without its waits (`report.h`). What is given up, in order:

1. **The sun's shadows**, where the player left them to the app. Off at the start of a run; on once thirty seconds held 97% of their rate (thirty, since a level's opening view is its lightest: with ten the Thor at 120 was given them in Seoul and S.S.D.D. for the seconds until the first heavy view took them away) with the scene whole, the chip under 55% of a frame's time and the renderer's thread under 60% (the shadows cost a fifth to a third of both); off again when four seconds come under 90% of the rate with them, or the moment the scene would otherwise be drawn smaller. Not tried again for a minute, then two, four, up to sixteen, when they went within a minute of coming on.
2. **The scene's pixels** (`MW2_SCENE_DYNAMIC`, [rendering.md](rendering.md#the-scene-drawn-smaller-under-load)): on every chip now, in the titles and at the sizes that have it (not under the internal render multiplier), and down to 58% of its width where it stopped at 70%. An Adreno 6xx still starts from 832x480: the title makes its surfaces before a frame can be measured.
3. **The frame rate**, where the player has chosen no limit (the FPS limit's **Auto**, which a fresh install has; `MW2_FPS_AUTO`). The run starts at the screen's fastest rate and may be held to any rate the screen shows evenly -- every mode's rate and its half, down to 30 (`Prefs.fpsSteps`, `MW2_FPS_STEPS`: 120, 60, 30 on the Thor; 60, 30 on the Flip 2). Down after ten seconds in a row under 90% of the rate, with the scene already at its smallest or the chip not what is late, to the slowest step that gives up no more than 12% of what was being drawn: 62 frames a second under 120 become an even 60, 85 stay as they are, 25 go straight to 30. Up a step after thirty seconds with the chip and the renderer's thread under 70% of the faster step's frame; a step up that does not hold for a minute is tried again after two, four, up to sixteen. The player is told in a line on the screen.

A limit the player sets, in Settings or the game's menu, stays whatever the device does, and so do shadows set On or Off; Auto in either place hands them back. Every change is a line of the log (`adaptive: held to 60 frames a second, not 120: 10 s at 52 frames a second under 120 (the window: ...)`), and once a minute of play a `performance:` line sums the run up, since a run the player ends is killed and says nothing at its end. The rule itself has no clock and no device in it and is checked on windows nobody measured (`tests/adaptive_test.cpp`).

**The display's mode** goes with the rate or does not, whoever set the rate (`Governor::Display`, `GameActivity.followHeldRate`). A rate the display has a slower mode for is shown there a frame a blank, evenly and for less power (`DisplayRate.request`: the slowest mode that shows it). But the slower mode is not only the display's. With eight busy shells beside the game an AYN Thor's system gives the game 16 to 17% of the processor while the display runs at 120 Hz and 8 to 9% in its 60 Hz mode, whichever build it is and whatever put the display there, and the mixer then makes 60 to 75% of the sound in time. Seoul under that load: 111 underruns in a minute of play in the release build at 120 Hz, 2,427 in the release build with a limit of 30 chosen, and on this change before this rule 1,269 with a limit of 30 chosen and 1,208 once Auto had held the run to 60 and the display had followed. So:

- held with the **graphics chip** late (85% of a frame's time or more when the rate went down), the display follows the rate: a run the chip holds back loses nothing in the slower mode;
- held with the **processor** late, the display keeps its fastest mode and each frame stays for two of its blanks or four, until the rate is back at its first step;
- and whatever put the display in a slower mode, a limit the player chose as well, it is asked for its fastest again, for the rest of the run, once the sound has broken up there: two underruns a second for six seconds in a row (14 to 49 a second on the Thor under that load, one in two seconds at the most where the sound is whole; a level starting stops the mixer for a second or two).

The log says which: `adaptive: held to 60 frames a second, not 120: ...; the display follows the rate`, `...; the display is kept at 120 Hz because the processor is late`, `adaptive: the display is kept at 120 Hz because the sound broke up in a slower mode (its 60 Hz mode: 5 underruns in the last 2 s)`; the performance report has a `display mode:` line. What keeping the mode costs, measured on the Thor with Seoul held to 60 and nothing beside it: the frames are not as even, since the guest's 60 Hz clock is not the panel's 119.1 Hz halved -- 87 to 89 of 97 frames were on screen for 16.7 ms and the rest in pairs of 8.3 and 25.0, where the 60 Hz mode had 97 of 97 -- and the phone drew 872 mA from USB where it drew 751 in the 60 Hz mode (25 samples each, of 340 to 1,750 mA, and the 60 Hz run had the sun's shadows on): about a sixth more, which is within what the samples spread.

**Measured on the two devices, 26 November 2026** (`.dev` debug builds, the bundled Turnip). The first four rows are the visual suite's own figures, on `821d28f` before and on this change after; the loads are 150 s runs of the suite's Seoul.

| | before | now |
|---|---|---|
| Thor, Advanced Warfare's Seoul | 118.3 | 118.3; the shadows not on: no thirty seconds in a row had the room for them |
| Thor, Modern Warfare 2's S.S.D.D. | 118.7 | 118.6 |
| Flip 2, Seoul | 60.4 | 60.4 |
| Flip 2, S.S.D.D. | 60.1 | 60.1 |
| Thor, Seoul at three times its size (the chip 19 to 38 ms a frame) | 38.5 under a limit of 120 | held to 60 after ten seconds at 52, to 30 after ten at 27; the display follows the rate into its 60 Hz mode; no underrun |
| Thor, Seoul with eight busy shells (the processor 98% busy) | 45.6 under 120; 152 underruns in 82 s of play | held to 60 after ten seconds at 65, to 30 after ten at 26; the display kept at 120 Hz because the processor is late; 1 underrun in 106 s of play |
| Thor, the same, the shells stopped 110 s into the run | | held to 60 at 66 s and back to 120 at 155 s: thirty seconds with room for it, once the minute after the step down had passed; 3 underruns in 256 s of play |
| Thor, the same shells, an FPS limit of 30 chosen (the display in its 60 Hz mode) | 2,427 underruns a minute of play | the display back at 120 Hz 16 s into the run because the sound broke up; 7 underruns in 67 s of play |
| Flip 2, Seoul at twice its size (the chip 33 to 38 ms) | 28.2 under 60 | held to 30 after ten seconds at 28 |

The visual suite on this change (rebased on `0120f88`, both phones, the ten scenarios each): the Thor passed all ten at 117.4 to 119.1 frames a second (117.2 to 119.2 on `821d28f`); the Flip 2 ran 59.7 to 60.4 with two failures that are not the picture's. `mw2-bots`, `local-flicker` at 101.2 s: a near pillar leaving the left edge of the picture in a turn, there the frame before and gone the frame after. `mw3-campaign`, `black` at 149.0 s: the walk took the player into an unlit doorway in Black Tuesday for nine seconds, the weapon, the sight and the HUD drawn over the dark, which the check takes for a black picture; on `821d28f` the walk did not end there.

**The performance report.** The game's menu has **Share performance report**: the app, the phone, the chip and its memory (`DeviceInfo`), the screen's modes, the settings and which of them were chosen (`Prefs.graphicsSummary`), and from the run the driver, the device line of the log, what the run is held to and whether the display follows it, the play so far (frames a second, the slowest two seconds, the share of the time under the rate, the chip's and the renderer's thread's time a frame), the last minute ten seconds a line, the last changes, the sound's underruns and what its sink holds, and the heat (`nativePerformance`, `adaptive::Report`). It goes as the launcher's log does (`LogShare`): a text file through Android's share list, or its end copied; and it is written into the run's log as well.

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
- **A first visit** (`MW2_ASYNC_PIPELINES`, on in the app): a pipeline never built is compiled by up to six threads on the performance and the efficiency cores ([below](#a-first-visits-pipelines)) while its draws are skipped; the renderer's own thread compiles only those of draws that write no colour (shadow casters, the depth pass: 9 in a first Rust match, 32 to 40 ms in all), which are never skipped, and waits up to 32 ms for a pipeline or two alone so that nothing already on screen blinks ([rendering.md](rendering.md#pipelines)). On the AYN Thor with empty caches the surfaces of a first spawn are all there within about three seconds and no frame is held (the longest 68 to 83 ms in Seatown, Bakaara, Afghan and Survival); a second visit compiles nothing it drew before ([rendering.md](rendering.md)).
- **Signal Safety & Crash Guarding (`runtime/crash.cpp`)**:
  Bionic libc lacks `pthread_kill` validation for arbitrary guest thread IDs. Long-running compilation stages are protected with Android-specific signal guards (`#ifndef MW2_ANDROID`), preventing spurious `SIGABRT` crashes.

---

## 5. Controls & Virtual Gamepad

### Virtual Touch Overlay (`TouchOverlayView.kt`)
The virtual gamepad converts multi-touch gestures into normalized synthetic XInput controller packets:
- **Movement (Left Stick)**: Analog thumbstick with customizable deadzone.
- **Looking**: the right half of the screen, outside the buttons, turns the view by the finger's own movement ([Looking by touch](#looking-by-touch) below). A right stick is in the layout, hidden, for whoever wants one.
- **Sticky ADS**: The Aim Down Sights button can be configured to toggle or hold.
- **HUD Layout Editor**: Open the game's menu and choose **Edit controls** to move, resize, adjust opacity, or hide any button. Layouts are saved in `filesDir/controls.json`.
- **Off**: Settings → Controls and the game's menu both have **On-screen controls**. Off stays off across starts and updates (below); the Menu button at the top right stays, and a controller opens the menu with Select and Start.

### Graphics settings: the presets, the Advanced page and the counter (`GraphicsPresets.kt`, `Prefs.kt`)
**Settings → Graphics** starts with five presets, each with one line saying what it does, and an **Advanced…** button. The same in all four games.

A preset is a value for each setting of the Advanced page and nothing else (`GraphicsPresets.values`), so the page always shows what the preset is, and there is no second mechanism beside the runtime's switches ([switches.md](switches.md)):

| Preset | Resolution scale | Frame-rate limit | Sun shadows | Render multiplier | Dynamic resolution |
|---|---|---|---|---|---|
| **Automatic** (a fresh install) | 70% | Auto: the screen's rate, held lower where the device does not keep it (`MW2_FPS_AUTO`) | Auto: measured (`MW2_SHADOWS` unset) | 1x | On (`MW2_SCENE_DYNAMIC`, the runtime's default) |
| **Battery saver** | 70% | 30 (`MW2_FPS_LIMIT=30`) | Off (`MW2_SHADOWS=0`) | 1x | Always smaller: 70% of the scene's width, half its pixels (`MW2_SCENE_SHARE=70`) |
| **Balanced** | 70% | 60 | Auto | 1x | On |
| **Best looking** | 100% | Auto | On (`MW2_SHADOWS=1`) | 1x, or 2x once measured (below) | Off: always whole, and the console's 1024x600 on an Adreno 6xx too (`MW2_SCENE_DYNAMIC=0`, `MW2_SCENE=console`) |
| **Custom** | what the Advanced page holds | | | | |

- **The player's choice always wins**, as before: a limit of 30 or 60 from a preset is a limit the player chose, which the runtime holds and does not step (`adaptive::PlayerSetLimit`), and shadows On or Off are not measured. Only Auto leaves a thing to `runtime/adaptive.h`.
- **Custom.** Changing a setting of the Advanced page by itself, there or in the game's menu, makes the preset Custom (`Prefs.custom`). Choosing a preset writes all five settings in one edit; what the player had set by hand is kept and comes back when Custom is chosen again (`Prefs.choosePreset`). An install older than the presets is the preset its settings are: Automatic where nobody changed them, else Custom (`GraphicsPresets.matching`).
- **Best looking at twice the size.** Twice the width and height costs the graphics chip about 2.25 times the time (Advanced Warfare on the Thor: 3.6 to 7.6 ms a frame in a match, 4.8 to 10.8 in the lobby). So the preset starts at 1x, and a run under it of a minute of play or more leaves the chip's time a frame behind (`GameActivity.measureBestLooking`, from `adaptive::Current`). The next start of that title draws at 2x only where 2.25 times that time is within 85% of a frame at the screen's fastest rate (`GraphicsPresets.twiceHolds`; 85% is where the runtime calls the chip late). A run at 2x of which more than a tenth came late, or which the runtime held to a slower rate, puts the title back to 1x for good (`twiceFailed`). The campaign and the multiplayer are measured apart. The log says each: `graphics: best looking measured the graphics chip at … ms a frame`, `graphics: best looking at 2x did not hold`.
- **The Advanced page** (`SettingsActivity.bindGraphics`, in the Graphics page's place): resolution scale (35 to 100%), frame-rate limit (Auto, then 30, 60, 90 and 120 as far as the screen has a mode that fast: `Prefs.fpsLimits`; a limit of 45 saved by an older version stays and is shown), sun shadows (Auto, On, Off), render multiplier (1x to 3x), dynamic resolution (On, Off, Always smaller) and the graphics driver (the Turnip the app carries or the phone's own; **Other drivers…** opens the import screen). Each has one line under it, which says where a restart of the game is needed: the render multiplier, dynamic resolution and the driver. **Reset to automatic** puts all six back as a fresh install has them (`Prefs.resetGraphics`): the Automatic preset, the driver the app would choose by itself, and what Best looking measured forgotten.
- **The log** of a run starts with the preset and the settings in force (`graphics: preset balanced`, `graphics: resolution 70%, internal multiplier 1x, 3D scene follows the load, …`, `graphics: FPS limit 60 (chosen), sun shadows auto (measured), …`), before the `android: MW2_…=` line of each switch.
- **The counter** (Settings → Graphics → **Show the performance counter**, and **Performance counter** in the game's menu; on until turned off): one small line in the top left corner, `118 FPS · 8.5 ms · held by the graphics chip`. The frames a second are the frames presented, the time is one frame's, and the last word is what the frames of the last two seconds of play waited for, as the runtime measured them (`adaptive::Governor::WhatWasLate`, the rule the governor itself steps the rate by): **held by the limit** where they came at nine tenths of the rate the run is held to or more, **held by the graphics chip** where they did not and the chip took 85% or more of a frame's period by its own clock, **held by the processor** where it took less. It says nothing in a menu, a film or a loading screen, which are no measure of the device, nor on a chip that keeps no time. While shaders are still being compiled it says how far that is, and in a multiplayer how many other players were heard on the network (`LAN: 2`). A tap opens the card with the rest (temperatures, memory, the driver, the scene's size). The log's line a minute says the same of the minute: `performance: … the frames waited for the graphics chip 12% of the time and for the processor 0%`.
- **Tried on both phones** (27 Nov 2026, each preset chosen by touch in Settings, then the campaign scenario of tools/visualqa for two minutes, the mean frames a second of 40 to 48 s of play): on the AYN Thor (120 Hz) Battery saver 30.8 to 30.9, Balanced 60.5 to 60.9, Best looking 117.6 to 119.9 and Automatic 119.5 to 119.6 in all four games; on the Retroid Pocket Flip 2 (60 Hz) Battery saver 30.6, Balanced 60.5 to 60.7, Automatic 60.6 to 60.7, and Best looking 57.8 (Modern Warfare 2), 58.9 (Modern Warfare 3), 59.1 (Call of Duty 4) and 55.9 (Advanced Warfare, whose Seoul street fell to 33 to 45 for ten seconds at the full scene with shadows). Best looking stayed at 1x on both: the chip took 6 to 7 ms a frame at 1x on the Thor and 12 to 20 on the Flip. The counter's line matched the log's seconds, Reset to automatic put every setting back, and the game's menu changed the preset in play (Automatic to Battery saver: the log's limit went to 30 within the second, and back).

### Looking by touch
A player without a controller (Call of Duty 4, a Snapdragon 685 phone) wrote that "the screen sensitivity is bad" and that he did not like "relying on a joystick touch control". This is what was wrong and what it is now, in all four games.

**How it works.** The half of the screen the look area is in (the right, unless it is dragged across the middle in **Edit controls**) turns the view wherever a finger lands beside the buttons. A movement of the finger is an angle: the setting is degrees for a centimetre of glass (13 to begin with; 3 to 40), which `TouchLook.kt` makes degrees a pixel from the density the phone draws at, with a share of it for up and down, a share while the left trigger (Aim) is held, and an inversion of up and down. `runtime/android/input.cpp` hands the degrees to the hook `runtime/mouse_look.cpp` has on the title's `AimAssist_UpdateGamePadInput`, the function `CL_GamepadMove` gives the view's two angles to once a frame: they are added to the yaw and the pitch that function returns, times the zoom factor the title keeps for its own stick's turn rates (1 from the hip, smaller down a sight), which is how the PC games scale a mouse. The stick, its dead zone, its response curve, its acceleration and its turn rate are not in the way, and nothing is smoothed: what the finger did before a frame is in that frame. The hook is the one the mouse has on Windows; its addresses were found for Call of Duty 4 and Advanced Warfare (`runtime/title.h`), so every executable of the four games on a phone has it: nine, with Advanced Warfare's title-update multiplayer. A hook is not part of what a game library is built against ([game-library.md](game-library.md)), so no game code is built again. Where the title is not working a view out, the degrees become a stick as the mouse's do on Windows.

**Firing while looking.** A finger that went down on a button or a trigger on the look area's side (Fire, the face buttons, RB, Melee) and is dragged more than 8 dp turns the view as it goes, the button still held: the thumb on Fire goes on aiming. **Tap the look area to fire** (off to begin with) makes a touch of under a fifth of a second that did not move a shot. The right stick that stood in the middle of the look area is hidden in a new layout and in one saved before this (`controls.json` version 2), and comes back in **Edit controls**; the face buttons, Fire, RB and Melee were moved a little so that none lies over another or runs off a 16:9 screen 360 dp high (`ControlLayoutTest`: 640 by 360, 800 by 360 and 731 by 411 dp; at least 70% of the look area is free of buttons on each).

**The gyroscope** (off to begin with; always, or only while Aim is held). `GyroInput.kt` reads the game rotation vector (gyroscope and accelerometer, no compass) 200 times a second on a thread of its own, and `GyroLook` turns two attitudes into a turn: sideways is the part about the world's vertical however the phone is held, up and down the part about the screen's left-to-right axis. Under a degree a second it is scaled towards nothing, so a phone lying still does not creep. It is added to the same hook, with a sensitivity of its own (degrees of view for a degree of phone), and works with a controller too. `TouchLookTest` has the arithmetic; nobody has yet played with it in hand.

**Where the settings are.** Settings → Controls, and in a game **Menu → Touch look and gyroscope**, where each is taken as it is changed (`nativeLookSettings`). The help over the game's first run says how to look and fire. A physical controller is untouched: its sticks go the way they did, and the on-screen controls are hidden by the same switch as before.

**What it was (the audit, 27 November 2026).** The look area was already a swipe area, not a stick on the glass, but what it sent was a stick: the finger's speed over 2200 pixels a second as the right stick's deflection, held to full tilt, 65% of each new value and 35% of the last (a smoothing), pushed past a dead zone of 14%. The title then did to it what it does to a stick: its response curve, 1200 degrees a second squared of acceleration, 260 degrees a second at most. Measured with `tools/touch_look.py` in Call of Duty 4's The Bog at 60 frames a second, swipes of 2 cm (26 degrees at today's default) turned the view 3.2 and 1.8 degrees over two seconds, 2.7 and 3.4 in 0.3 s and 0.5 and 0.4 as a flick on the AYN Thor: 0.2 to 1.7 degrees a centimetre by how fast the finger went. On the Retroid Pocket Flip 2 the slow swipe turned it by nothing at all and the others by 0.04 to 0.7 degrees. A millimetre a second did nothing on either, and ten swipes there and back left the view 1.8 and 3.2 degrees aside.

**What it is.** The same swipes with this build, the setting at 13 degrees a centimetre (a centimetre is 145 pixels on the Thor and 142 on the Flip, whose densities are 369 and 360; the Flip's panel is 401 pixels an inch, so its true figure is 14.5 degrees for a centimetre of its glass):

| 2 cm (asked 26.0°) | Thor, 60 FPS | Thor, 30 FPS | Flip, 60 FPS | Flip, 30 FPS |
|---|---|---|---|---|
| over 2 s, right and left | 26.04, 25.99 | 26.04, 26.08 | 25.96, 25.94 | 25.87, 25.91 |
| over 0.3 s | 26.04, 26.65 | 25.22, 26.04 | 25.70, 26.13 | 25.44, 25.27 |
| 1 mm over 1 s (asked 1.3°) | 1.33, 1.34 | 1.34, 1.34 | 1.27, 1.28 | 1.28, 1.27 |
| 1 cm down and up (asked 13.0°) | 13.19, 12.56 | 12.76, 13.04 | 13.11, 13.11 | 12.76, 12.76 |
| aiming at 50%, zoom 0.5713 (asked 7.4°) | 7.22, 7.19 | 7.29, 7.24 | 7.39, 7.27 | 7.22, 7.59 |
| Fire held and dragged 2 cm left, 1 cm down | 24.4 left, 12.1 down | 24.4, 12.1 | 24.3, 12.2 | 24.2, 12.1 |

The view's angles follow what the finger's movements asked for to the hundredth of a degree (the log has both), at either frame rate; ten seconds untouched it does not move. The same swipes in Call of Duty 4's multiplayer, Modern Warfare 3's and 2's campaigns and Advanced Warfare's multiplayer gave the same figures on both phones (the sight's zoom factor is 0.5713 in Modern Warfare 3's Black Tuesday, 0.6155 in Advanced Warfare, 0.732 in Call of Duty 4's multiplayer and 1 in S.S.D.D., where aiming at 50% turned 12.9 and 12.8 degrees for 13.0 asked). The Fire drag is 2 cm less the 8 dp that a pressed thumb may wobble.

**The system's guess, and the lift.** In that measurement a flick (2 cm in 0.06 s) turned the view 21.7 to 28.6 degrees for 26.0, and ten swipes there and back left it 0.1 to 4.1 degrees aside. The view had turned by exactly what the app was told (the log's movements and the view's angles agree); what the app was told was not the swipe: the last movement Android reports need not be where the finger stopped (it resamples a touch between frames, and a quick swipe ends between two of them). The position the finger came up at is now counted too (`TouchOverlayView.release`), so a swipe turns the view from where it went down to where it came up. Measured again with this build (27 November 2026, 60 frames a second): in Call of Duty 4's The Bog the flicks turned 26.04 and 26.04 degrees for 26.04 asked on the Thor and 25.96 and 25.96 for 25.96 on the Flip, the swipes over 0.3 s as much, and ten there and back left the view 0.000 aside on both; every other step of the run turned what it asked to the hundredth. The same on the Flip in Call of Duty 4's Crash, Modern Warfare 3's Dome and Modern Warfare 2's Rust (flicks 25.96, nothing left over; aiming at 50% turned 9.50, 9.09 and 9.50 for 12.98 asked, the sights' zoom 0.732, 0.7004 and 0.732), and in Advanced Warfare's campaign on the Thor (2 cm slow, brisk and as a flick 26.04 each; the Fire drag 24.5 left and 12.2 down). That campaign was measured on Bio Lab (`devmap lab`): the opening of Induction, the visual QA scenario's level, is the drop pod, whose view the player does not turn, and the log shows the view's angles not changing there at all. The wait from the screen's report to the view was 18 to 31 ms in all of these.

**Latency.** From the moment the screen reported a movement to the frame's view having it (`waited` in the log), averaged over each swipe: 18 to 32 ms at 60 frames a second and 30 to 48 at 30, which is the wait for the title's next frame and little more; that frame was on the Flip's screen 32 ms after it was finished (the run's own report). Before, a movement reached the view over several frames, through the smoothing and the title's acceleration; how many was not measured.

The measure is repeatable: `python tools/touch_look.py --device <serial> --apks <folder> --out <folder> cod4-campaign [--fps 30]` plays a visual QA scenario to where the level is in play with `MW2_TRACE_LOOK=1`, swipes with `adb shell input` and prints what the view did.

**Inverting (`LookInvert.kt`).** Settings → Controls and the game's menu have **Invert looking left and right** and **Invert looking up and down**; each flips that axis for a finger on the look area and for the gyroscope. The app signs the degrees before the runtime takes them (to the right and downwards are positive; `nativeLookSettings` has no invert), so the angles written into the view (`mouse_look.cpp`) are the player's. The titles' own Invert option (`input_invertPitch`, `profile_toggleInvertedPitch`; Modern Warfare 2 and 3, Call of Duty 4, Advanced Warfare, Black Ops III have it for up and down and none for left and right) acts on a controller's stick only, never on these angles; so a third switch, **Controller: invert the right stick left and right** (`GamepadInput.invertRightX`), is the app's, off to begin with, and up and down on a controller is left to the game's option (the setting's hint says so, so it is not inverted twice). The one switch of older versions (`lookInvert`) is read as Invert up and down until the new one is set. `python tools/invert_look.py --device <serial> --apks <folder> --out <folder> cod4-campaign` plays each setting on a phone and checks the `look:` lines turn the way the switches say.

### Aim when firing (touch)
Asked for by a player who plays with touch only and has two thumbs for four jobs: **Settings → Controls** (and **Menu → Touch look and gyroscope** in a game) has **Aim when firing (touch)**, off to begin with. It is the on-screen Fire button only (`TouchOverlayView`, rules in `AimWhenFiring.kt`, `AimWhenFiringTest`); a physical controller, the tap on the look area and the Aim button itself are as they were.

- **Fire down**: the left trigger (Aim) is held at once, and the right trigger (Fire) **Aim before the shot** later: 200 ms to begin with, 0 to 500 in steps of 50. The Aim button lights while Fire holds it.
- **Fire up**: both let go together. A tap shorter than the delay still shoots: the right trigger comes at the delay and is held for at least 70 ms (two frames of a title at 30 a second, so the title's own poll sees it), and then both go up. Fire pressed again while that is still held keeps the sights and fires at once.
- **Aim already held or toggled on** when Fire goes down (Aim is a toggle in the default layout): nothing is added. Fire is the right trigger at once, as without the setting, and the sights stay the player's: pulling Aim off while Fire is down ends the aim and not the shot, and the Aim toggle, if it was on, is still on when Fire comes up.
- **Looking while firing** is as it was: the finger on Fire that is dragged more than 8 dp turns the view (and the sight's slower turn rate follows the left trigger, which is now held). Fire as a toggle (a layout from the editor) holds from one tap to the next in the same way.
- **Hip fire** is gone while the setting is on: Fire always aims. At close range turn it off in the menu (one row) or set the delay to 0, which fires as the sights start up. **Sniper rifles** are the reason for the delay setting: their scope takes longer than a rifle's sights, and the default shot leaves while it is still coming up; give them 300 to 400 ms, or hold Aim yourself, which Fire then leaves alone. Nothing here knows the weapon.

**How long the sights take, measured.** In Call of Duty 4's campaign (The Bog, the M4 with the holographic sight, the Retroid Pocket Flip 2, 30 frames a second): the game's poll sees the left trigger 10 to 25 ms after the finger and the sight's zoom factor starts to fall some 100 ms after that, reaching its value (0.5713) at about 240 ms after the trigger (1.0, 0.99, 0.84, 0.70, 0.58, 0.57 at 91, 118, 156, 185, 223 and 256 ms after the finger). So a shot 150 ms after the trigger leaves with the zoom about half the way and one at 200 ms with it 96% of the way. The other titles were not measured: they get the same setting and the same default, and the zoom factor in `MW2_TRACE_LOOK=1`'s `look:` lines is how to measure them.

**First shot, measured** (`tools/aim_fire.py`, the same phone and scene, the default 200 ms, 27 November 2026): with Fire held the game's poll saw the left trigger 14 and 24 ms after the finger and the right trigger 214 and 223 ms after it (201 and 199 ms after the left one; a frame is 33 ms), and let go of both together 3 and 20 ms after the finger came up. A tap of 40 ms: left trigger at 24 ms, right at 224, both let go at 291 (the shot held 67 ms). With Aim already toggled on: the right trigger at 8 ms, the left one never changed and was still held when Fire came up. With the setting off: the right trigger alone at 18 to 24 ms. A picture with Fire held shows the holographic sight up, the muzzle flash and both the Aim and the Fire button lit (`hold-on.png` in the run's folder).

`python tools/aim_fire.py --device <serial> --apks <folder> --out <folder> cod4-campaign [--fps 30]` plays the scenario to where the level is in play and puts a finger on Fire with `adb shell input`: held, tapped (30 ms), with Aim toggled on first, and dragged, then the same with the setting off. It sets three records on the phone's uptime clock side by side: the debug build's `TouchTrigger` line in logcat (when the finger came down and up), the game's `trigger: lt … rt … at … ms` line in `game.log` (what its poll saw, runtime/android/input.cpp, with `MW2_TRACE_LOOK=1`) and the `look:` lines' zoom factor and time. Exit 1 when an order or a time is wrong.

### The movement stick: floating, and auto-sprint (`MoveStick.kt`)
Asked for by the same player and by issue 1 of the aw-recomp repository: two switches, both off to begin with, in **Settings → Controls** and in a game **Menu → Touch look and gyroscope** (`Prefs.floatingStick`, `Prefs.autoSprint`). The right half of the screen is already a swipe-to-look area, so the "dynamic stick" of Call of Duty: Mobile that they mean is the movement stick on the left, and it is that one that floats.

- **Floating movement stick.** A finger that comes down on the movement stick's side of the screen (the half its resting place is in) where no button is takes the stick, and the stick is put down under it: the base is drawn there, the knob follows the thumb, and the push is measured from where the thumb came down (`MoveStick.deflection`), out to the stick's own radius (75 dp in the default layout) and no further, as a fixed stick's is. The base stays where it was put and is not drawn while no finger is on it; the stick's own place is not a place any more while the switch is on, so a thumb anywhere on the left finds it. A button under the thumb is still the button (the D-pad, Run, Aim, LB, and in **Edit controls** the stick is drawn and moved as ever); a second finger on that side while the first has the stick does nothing. Lifting the thumb puts everything back to the centre.
- **Auto-sprint.** While the movement stick is pushed to 90% of its radius or more, a third of a second later the game's sprint, the left stick's click, is pressed for the player; pulled back below 70% (or the thumb lifted) it is let go. A thumb that wobbles between 70 and 100% neither starts nor stops it, and a flick across the pad shorter than the delay does nothing (`AutoSprint`, `MoveStickTest`). The Run button (L3) still works by hand, and lights while the stick is holding it. It is held, not tapped: the titles end a sprint themselves when the player stops, fires or aims, and the click held down is what a thumb on the real stick does; whether a title would rather have a tap (a toggle in a title's own options) was not tried in each.

`python tools/move_stick.py --device <serial> --apks <folder> --out <folder> cod4-campaign` puts a finger on the glass with `adb shell input motionevent` (down, move, up, so that it can stay still and come back) and sets the debug build's `TouchStick` line in logcat (where the stick was put down), the game's `stick: lx … ly … l3 … at … ms` lines (the left stick's push as the game's poll saw it and the click of the stick, `MW2_TRACE_LOOK=1`) and pictures side by side; and the same touches with both switches off.

**Measured** (`tools/move_stick.py`, the Retroid Pocket Flip 2, Call of Duty 4's campaign, 30 frames a second, 27 November 2026): a thumb put down at pixel (384, 594) and another at (633, 486) made two bases there, to the pixel, and the pad's stick stayed at the centre while still; pushed half a radius to the right it read 0.50. Pushed past the edge and held, the stick read 1.0 at 454 ms and sprint was pressed 299 ms later (300 asked); pulled back to 0.40 it was let go, and the thumb coming up put the stick back to 0. The edge held for 100 ms pressed nothing. With both switches off the same touches put down no stick, moved nothing and pressed nothing (outside the buttons the left half of the screen is not a control). A picture with a thumb down shows the base under it with the knob pushed to one side.


### The menu over the game (`GameMenu.kt`)
One panel of the app's own in every title (Modern Warfare 2, Modern Warfare 3, Call of Duty 4, Advanced Warfare, campaign and multiplayer), at the left of the picture with the game running beside it, so a setting is seen as it changes. It is not the title's pause menu: those are four menu systems in the games' fastfiles.

- **Opening it**: the **Menu** button at the top right, the back key or gesture, or **Select and Start together** on a controller (either order; the second of the two is not passed to the game). The D-pad or left stick chooses a row, left and right change its value, A takes it, B goes back, Start closes it. By touch: a row, or the arrows beside its value. While it is up the game sees no pad input.
- **Graphics**, each written to the same `Prefs` the launcher's Settings show, the preset first:

  | Row | How the running game takes it | |
  |---|---|---|
  | Preset | the preset's frame-rate limit, sun shadows and resolution scale at once, as the rows below; its render multiplier and dynamic resolution at the next start. The page is shown again with the preset's values, and the preset's line under it | live, but for the two marked rows |
  | Resolution scale, Sharpening, Fill the screen | Sharpening is `vk::sharpen::Set`; the surface is given another size (`SurfaceHolder.setFixedSize`), the presenter makes its swapchain again; the title's own frame is untouched | live |
  | Frame-rate limit | Auto (`adaptive::PlayerSetLimit(0)`: from the screen's rate, and under it what the device keeps, [above](#what-a-device-is-given-and-what-it-is-measured-for)) or a number, which stays. `frame_rate::SetLimit`: the pacing follows at the next present (`d3d_hooks.cpp`, `Pace`), the surface votes again for the display's rate and the window asks for the mode; back on the console's 60 the title's `com_maxfps` and `r_vsync` are reset to its own | live |
  | Sun shadows | `sm_enable` at the next present (`live::SetSunShadows`); Auto is what the frames are measured to have room for (`adaptive.h`), asked again at every present | live |
  | Field of view | Modern Warfare 3's multiplayer and Advanced Warfare: `MW2_FOV`'s scale (`live::SetFieldOfView`), which a spawn does not undo; the others: the title's `cg_fov` | live |
  | Detail at distance | Modern Warfare 2's disc version only (`live::SetDetailBias`) | live |
  | Texture memory | the texture cache's budget (`android::perf::SetTextureBudgetMB`) | live |
  | Performance counter | the overlay's view | live |
  | Render multiplier, Dynamic resolution, Smooth edges | `MW2_SCALE`, `MW2_SCENE_DYNAMIC` with `MW2_SCENE` or `MW2_SCENE_SHARE`, and `MW2_NO_MSAA` size the renderer's targets and the title's scene when they start | marked `*`: the next start |
  | Graphics driver | the phone's own or the app's (and an imported one while it is the one in use); a process opens its driver once. Only where the build carries a driver for the chip | marked `*`: the next start |

- **The rest**: On-screen controls and Physical controllers (on or off), Edit controls, the multiplayer's invitations, bots (`bots <n>`) and Modern Warfare 2's stat cheats, and Quit, which asks on a page of its own.
- **One settings file for two processes** (`Prefs.kt`): the launcher and the game are separate processes and both write the `mw2` preferences. A process reads a preference file once and afterwards writes its own copy whole, so a choice made in the game (the on-screen controls hidden) was undone by the launcher's next write, and came back at the next start. The file is now opened with `MODE_MULTI_PROCESS` at every use, which reads it again when the other process has written it, and the game waits for its writes to reach the file before its process is killed (`Prefs.flush`).
- **`nativeApplyOption(key, value)`** (`runtime/android/jni.cpp`, `runtime/live_options.h`) is the one call behind the live rows; it returns false for a switch that is read once at the start of a run. The environment is never written under a running title.

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

### Older Snapdragons (Adreno 610-619, Android 10 and 11)

No phone of this class has run the game; what follows was checked on the PC and on the two Android 13 phones ([older-hardware.md](drafts/older-hardware.md) has the research).

- **Every imported driver crashed at `vkCreateInstance` on Android 11** (a Realme 5 Pro's log, 0.1.12): libadrenotools' linker-namespace code keeps function pointers in globals named `android_get_exported_namespace`, `android_link_namespaces` and `android_link_namespaces_all_libs`, and has a wrapper named `android_create_namespace`, the names of the linker's own functions in `libdl_android`. Linked into `lib<title>.so` with default visibility, and with `-z global` putting that library ahead of the others, they were exported, and a system library that asks the linker for `android_get_exported_namespace` got our 8-byte variable and jumped into `.bss`: the faulting address was exactly that symbol. `cmake/android.cmake` now links `libadrenotools.a` and `liblinkernsbypass.a` with `--exclude-libs` into every title's library, and `liblinkernsbypass.a` the same way into `libhook_impl.so`, so none of those names is exported any more (`readelf --dyn-syms`). `libmain_hook.so` still exports `android_dlopen_ext`: it is the hook, loaded only in the driver's namespace, and the driver's calls are meant to reach it.
- **The bundled Turnip was only given to Android 12 and later**: the app knew a Snapdragon by `Build.SOC_MANUFACTURER`, which Android 12 added. Below it `QualcommSoc` reads `Build.HARDWARE` (`qcom` from Qualcomm's board support package), `Build.BOARD` (a Qualcomm platform: `sdm710`, `sm6150`, `msmnile`, `atoll`...) and the OpenGL ES renderer the app already logs (an Adreno, or the vendor Qualcomm), none of which a MediaTek, Exynos, Tensor, Unisoc or Kirin phone says (`QualcommSocTest`, with the values of a Realme 5 Pro, a Pixel 4a, a Poco M3 Pro 5G and a Galaxy A51). A player who had already chosen a driver gets the bundled one unpacked but not selected, and the black screen's message offers it in place of an imported driver that failed.
- **No community Turnip loads on Android 10.** They are compiled for API 30 or later and ask for bionic's `LIBC_R` (C11 threads), whatever their `meta.json` says. Android 10 is given the project's own build instead: Mesa `483aff363be`, unpatched, compiled for API 29 by `android/build_turnip.sh` (run by `android/fetch_deps.sh`), whose newest libc version is `LIBC_P`; two builds in different folders gave the same zip byte for byte. Android 11 and later keep Turnip v27.0.0-R2, the one the Thor and the Flip 2 were measured with. Selected by hand on the two Android 13 phones (27 November 2026, visualqa), the API 29 build passed Modern Warfare 3's Survival on both (117 fps on the Thor, 60 on the Flip 2) and Advanced Warfare's Induction on the Flip 2 (60 fps), as v27 did in the same runs.

### Snapdragon 8 Elite (Adreno 830) and other Adreno 8xx

A player's log (Xiaomi 24129PN74G, SM8750, Android 16, Qualcomm's driver V@0800.71 of 6 February 2026, Advanced Warfare 0.1.5): two runs on the phone's own driver ended the same way, with SIGSEGV inside `/vendor/lib64/libllvm-qgl.so`, the driver's LLVM shader compiler, called from `vkCreateGraphicsPipelines` on the thread that builds the pipelines draws wait for. Both times the last pipeline begun and never finished was `970E4A690F9729E6/D776FBA0455F46F8`, with the sun's shadows off (the Windows game draws that pair in Seoul). The registers held part of the compiler intrinsic it was handling, `…qgpu.load.uav…v4i32`: a vector load from a storage buffer.

What it is not, as far as the PC can tell: both shaders, recovered with `MW2_DUMP_SHADERS` from the Windows game and translated as the runtime does for that device (the arena as a storage buffer in the guest's byte order, pixel constants as a uniform block, one record a subgroup), pass `spirv-val`. The vertex shader has the same fetch code as `C770794A752D3FC1` (16-bit and 10:10:10 signed fields), the pixel shader uses no instruction `06FCBAEF5B6D1096` lacks, and those two pipelines compiled on that phone in 80-95 ms just before. No construct of ours stands out to rewrite, and a rewrite could only be tried on such a phone, so there is no shader-side workaround. The player's repackaged Qualcomm 512.842.6 runs the game, which also points at that driver version's compiler.

So an Adreno 8xx gets the project's own Turnip by default (`DriverStore.bundledAssetFor`, `DriverStoreTest` with that phone's values) in place of the community v27.0.0-R2 the 6xx and 7xx keep: R2 is Mesa of 7 November 2026 ("A7xx" in its own `meta.json`), before the a8xx fixes of 19-24 November (`d6007622331`, the gen8 VPC cache; `901c006e67b`; `791d6d0fafe`, QCTDD10982019), and a second player on an 8 Elite who cannot run the stock driver plays with a community a8xx build of 21 November. Ours, at `483aff363be`, has the Adreno 830's KGSL entry (`chip_id=0x44050001`, the device ID the player's Vulkan reported), the 840 and the other 8xx, and loads from Android 10 on. As for every Snapdragon, it is only selected for a player who has not picked a driver; one who has finds it in the driver list. Not run on any Adreno 8xx: the selection is unit-tested, the build reproduced (two builds, one zip), and the cause inside Qualcomm's compiler is not confirmed.

### Adreno 6xx (Snapdragon 865 class)

Measured on the Retroid Pocket Flip 2 (Adreno 650, kernel 4.19, 60 Hz), in Modern Warfare 3's Survival (Resistance). The runtime reads the chip's name from `/sys/class/kgsl/kgsl-3d0/gpu_model` before Vulkan starts (`android::perf::Adreno6xx`) and does these things on that class, and draws the scene smaller still while the chip is late with a frame (`MW2_SCENE_DYNAMIC`, [the campaigns' slow places](#the-campaigns-slow-places-24-november-2026)):

- **No UBWC.** Turnip is started with `TU_DEBUG=noubwc` (added to whatever `MW2_DRIVER_ENV` gives). With Turnip's image compression on, the whole device hard-reset within a second of a level's world first being drawn: no kgsl fault in the kernel log, the power controller reporting a shutdown the SoC asked for (`kernel.panic_on_oops` is 1 and there is no pstore, so the kernel's last words are lost). It happened at every one of a dozen level starts, with Mesa 25.1.0 and 27.0.0 alike, with `sysmem`, with draw merging off and with multi-draw off, and at none of the eighteen since with `noubwc`. Which image class trips it was not narrowed down; frame time with and without it is the same. `MW2_ADRENO6_UBWC=1` leaves the compression on, to study the reset.
- **The cap starts at the screen's rate.** Before the texel forms this chip drew Modern Warfare 3 at 25 frames a second with the cap at 60 (38 ms of GPU a frame at the console's sizes), so the cap used to start at 30 here. With them Survival runs at 58 and Black Tuesday at 56 under a cap of 60 (the table below), so both titles now start at the display's fastest rate like any other phone, 60 on the Flip 2 (`Prefs.fpsLimit`); FPS LIMIT 30 in the launcher still gives the console's pacing at half rate ([switches.md](switches.md), `MW2_FPS_LIMIT`).
- **No sun shadows** (`MW2_SHADOWS`, unset): about 6 ms of GPU a frame less, which is what lets 60 be approached. `MW2_SHADOWS=1` in `.env` keeps them, best with FPS LIMIT 30. (Since 26 November 2026 unset is measured on every chip and not chosen by this one's name: here the frames seldom have the room, and the answer is the same, [above](#what-a-device-is-given-and-what-it-is-measured-for).)
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

#### The campaigns' slow places (24 November 2026)

The four campaigns on the Flip 2, `.f45` development builds, the app's defaults, `MW2_REPORT=1` with `MW2_REPORT_SECONDS=1` (a line a second: frames, frames on time, the chip's and the renderer's thread's time a frame), levels from the console (`devmap`), the player unhurt, one input script or a level's own ride. Logs and pictures: `perf-f45\` beside the repository. Three things made a level run at 40 to 50 frames a second there for seconds at a time:

- **The chip, in a level's heaviest views.** Modern Warfare 2's flight to the Gulag draws 5,400 to 5,700 draws a frame for half a minute: 17 to 22 ms of GPU where the display gives 16.7 (the 832x480 scene 9.1 ms, the frame's own three passes 3.0, copies and clears between passes 1.5). A 640x368 scene took 3 ms off the worst of it. Modern Warfare 3's Iron Lady waits 14 to 20 ms a frame for the chip for two to three seconds after each shell lands. Now the scene is drawn smaller for as long as the chip is late (`MW2_SCENE_DYNAMIC`, on for this class; [rendering.md](rendering.md#the-scene-drawn-smaller-under-load)).
- **A first visit's pipelines** (below): 41 to 45 frames a second for three to five seconds wherever a level cuts to a view it has not drawn before.
- **The renderer's thread**, in Call of Duty 4's Charlie Don't Surf where the helicopters land: 13 to 18 ms a frame of it, 51 to 57 frames a second for a second at a time. Not changed.

Not the heat: ten minutes of the Gulag held 60.0 frames a second with 95.8% of frames on time, the chip at 55 to 60 °C, the fastest core at 62 to 68 °C, and neither's clock was capped (670 MHz and 2.84 GHz throughout).

| Flip 2, cap 60: frames a second / frames on time / stutters | before | now |
|---|---|---|
| Modern Warfare 2, the flight to the Gulag (36 s of it) | 57.7 / 67%, its worst seconds 46 | 60.1 / 87%, its worst seconds 54 |
| the same level's first two minutes | 58.9 / 83.8% / 3 | 59.7 / 92.4% / 5 |
| Team Player's recorded flight | 59.8 / 89.0% / 21 | 59.8 / 90.4% / 23 |
| Modern Warfare 3, Iron Lady (two minutes, a shell every 12 s) | 58.3 / 83.6% / 116, 40 to 50 after each shell | 60.0 / 89.6% / 2 |
| Mind the Gap, a first visit | 59.2 / 93.0% / 72, five seconds at 41 to 45 | 59.9 / 93.1% / 20, its worst second 56 |
| Call of Duty 4, Crew Expendable | 59.9 / 93.5% / 18 (a first visit) | 60.0 / 95.3% / 8 |
| Charlie Don't Surf | 59.4 / 92.8% / 56 | 57.6 and 56.9 / 91% / 59 and 21, each with one or two reloads of a checkpoint (a shot friend) of 1 to 3 s |
| Advanced Warfare, Traffic, a first visit | 57.9 / 97.4% / 32, its first three seconds 31, 23 and 49 | 59.7 / 98.6% / 21, 56, 51 and 50 |
| Seoul's drop pod, Fusion's flight in | 59.9 / 95.1%, 60.1 / 94.1% (their first 40 s) | 60.0 / 96.9% / 13, 59.9 / 97.3% / 5 (170 s each) |

"Before" is the published build (`ec98da5`) but for the Gulag's, Team Player's and Iron Lady's rows, which are this build with `MW2_SCENE_DYNAMIC=0` (the published build's Gulag: 57.1 / 61% over the same 36 s, worst seconds 46). Eighteen more levels' openings were only walked through by the script, each once and as a first visit (Takedown, The Hornet's Nest, Of Their Own Accord, Wolverines, The Enemy of My Enemy, Whiskey Hotel, Loose Ends, Contingency, The Only Easy Day, No Russian, Second Sun; Hunter Killer, Bag and Drag, Scorched Earth, Goalpost, Persona Non Grata; War Pig, Shock and Awe): 58.8 to 59.9 frames a second, the seconds under 57 all at a cut to a new view (Second Sun, Goalpost and Persona Non Grata three to five of them at 35 to 48). What is further into those levels was not reached, and Cliffhanger's climb not at all. The scene was drawn smaller in 54% of the Gulag's frames (at 70% of its width in 37%), 36% of Iron Lady's, 11% of the ten minutes', and 3 to 13% of the others'; Traffic sat at 92% of its width throughout.

On the AYN Thor (120 Hz, where the scene keeps its size) nothing moved: S.S.D.D. 118.5 frames a second and 86.6% on time before, 118.4 and 86.6% now; Modern Warfare 3's Mind the Gap 119.0 and 89.5%, 119.0 and 89.6%; Call of Duty 4's F.N.G. 119.0 and 89.1%, 119.0 and 88.7%.

**The chip's time was read wrong.** The report's "graphics chip at work" counts each submission's time past the end of the one before, and once the chip's clock had been put back (once or twice a run on the Flip 2) it counted nothing until the clock had caught up with the old end: a minute at a stretch read 0.0 ms, and a run's average came out low (`gpu_time.cpp`; the report now says how often the clock stepped back). The scene's steps follow that time, so it had to be right.

#### A first visit's pipelines

A pipeline never built is compiled on threads of its own while its draws are skipped (`MW2_ASYNC_PIPELINES`). They were held to the performance cores, as the title's own threads are, and a Snapdragon 865 has three: two of them compiling left the title a core short. They now run on the performance and the efficiency cores alike, six of them (`MW2_COMPILE_THREADS`). Modern Warfare 3's Mind the Gap, caches emptied, where it cuts to the docks (about 300 pipelines):

| Flip 2 | frames a second there | frames late for the title | the longest a surface was in coming |
|---|---|---|---|
| two threads, performance cores (before) | 41 to 45 for five seconds | 66 | 180 frames |
| one thread, performance cores | 58 to 60 | 8 | 389 |
| two threads, every core | 55 for three seconds | 22 | 297 |
| four threads, every core | 58 to 60 | 6 | 261 |
| six threads, every core (now) | 56 to 60 | 9 to 14 | 212 to 218 |

On the AYN Thor the same first visit: 118.8 frames a second and 379 frames with three threads on the performance cores, 118.7 and 256 with six on every core; S.S.D.D.'s 579 frames and 349, at 118.6 and 118.2.

**Left.** The Gulag's worst three seconds are still 54 to 55: at 70% of the scene's width the chip takes 18 ms there, most of it vertices. The frame's own passes (3.0 to 4.3 ms of 1280x720 a frame in every title) and the copies between passes (1.2 ms) were measured and not changed. One first visit of Mind the Gap read 13.2 ms of GPU a frame at the chip's fastest clock in a view that takes 8.9 (and kept the scene at 76% of its width for it); it did not happen again in five more.

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
- **No sun shadows above 60, in every title** (`MW2_SHADOWS`, unset; the app's Sun shadows setting gives them back). This was the rule until 26 November 2026; unset is now what the frames are measured to have room for ([above](#what-a-device-is-given-and-what-it-is-measured-for)), which at 120 is the same answer but for a level's lightest views, for the reasons measured here. Modern Warfare 3 first: Under the limit of 120 the ring consumer is what a frame waits for (98% of its core at work, 2% of that waiting for the chip), and the shadow maps' passes are three in ten of the draws it parses. Five minutes each, caches full:

  | Modern Warfare 3, limit 120 | frames a second | frames on time | stutters |
  |---|---|---|---|
  | Survival (Resistance), with shadows | 106.0 | 36.6% | 1,291 |
  | Survival, without | 118.2 | 86.7% | 122 |
  | Black Tuesday, with | 114.9 | 78.1% | 490 |
  | Black Tuesday, without | 118.4 | 89.5% | 183 |

  A smaller scene does nothing for it with the shadows on (832x480: 106.2 frames a second in Survival's first minute against 107.1) and little without them (116.2 against 114.5), so the scene stays the console's there. Call of Duty 4 and Advanced Warfare are built as that title's kind and have had the same default since they ran here.

  Modern Warfare 2 kept its shadows at first, S.S.D.D.'s flight being 115.9 with them, and has the same default since 26 November 2026: the scenes that held it furthest from 120 are the ones the shadows cost most, in the ring consumer's draws and in the chip's time alike. The same build with `MW2_SHADOWS=1` and without, 100 to 160 s of play each, caches full (both with `MW2_PRESENT_AT_BLANK=1`, below; the table at the end of this section is without it):

  | Modern Warfare 2, limit 120 | frames a second | frames on time | seconds under 115 | stutters | GPU a frame |
  |---|---|---|---|---|---|
  | Team Player's three heavy views, with shadows | 93.0, 84.1, 81.2 | 0.9, 0 and 0% | all | | 9.6 to 10.4 ms |
  | the same, without | 105.8, 98.4, 93.9 | 33.4, 2.0 and 0.5% | all | | 7.1 to 8.0 ms |
  | S.S.D.D., with | 115.2 | 76.4% | 51 of 160 | 127 | 5.7 ms |
  | S.S.D.D., without | 118.6 | 88.3% | 6 of 160 | 39 | 4.5 ms |
  | The Gulag, with | 110.7 | 69.2% | 62 of 160 | 752 | 6.7 ms |
  | The Gulag, without | 112.6 | 83.0% | 23 of 160 | 150 | 5.3 ms |

  The picture is the title's own with `sm_enable 0`, looked at in each of those scenes: nothing is missing but the sun's shadows (S.S.D.D.'s camouflage nets cast none on the ground; the soldiers and the vehicles keep the shading the title gives them).
- **A screen the system holds at 60.** The Thor's display setting (`peak_refresh_rate` and `min_refresh_rate`, which its dual-screen assistant writes, and wrote 60 into after a restart) outranks an app's request for a mode, so the limit of 120 it starts with ran on a 60 Hz screen: 125 frames drawn a second by the title's limiter to show 60 of them. A limit above the display's rate is paced by the display now, one frame a blank ([switches.md](switches.md), `MW2_FPS_LIMIT`), and the app says once at the start of a run that the screen is set to 60 Hz and where to change it.
- **What is still under 120** (24 and 25 November 2026, `c9b669d`, the bundled Turnip, caches full, 100 to 120 s of play in each: campaign levels from the console with the scripted walk, multiplayer hosted with six bots; `MW2_REPORT_SECONDS=1`). Frames on time are those within 1.05 blanks of the one before, where 88 to 90% is what a run at the limit measures; the threads are in % of one core, and in brackets the share of the time a thread stood ready with no core to run on.

  | | frames a second | on time | GPU a frame | ring consumer | recorder | main thread | what holds it |
  |---|---|---|---|---|---|---|---|
  | MW2, S.S.D.D. | 118.0 | 84.4% | 5.6 ms | 58 | 19 (21) | 35 | at the limit but for seconds at 105 to 114 |
  | MW2, Team Player's three heavy views | 79.0 to 81.6 | 2.7% | 10.0 to 10.3 ms | 96 to 97 | 27 (35 to 38) | 44 (14 to 16) | the ring consumer, 11.8 to 12.6 ms a frame; then the GPU |
  | MW2, The Gulag | 113.1 | 74.2% | 6.6 ms | 70 | 26 (28) | 41 (12) | 20 s of 120 at 76 to 86: the consumer 11.2 to 12.8 ms and the GPU 9.8 to 10.9 ms a frame there |
  | MW2, Just Like Old Times (`af_caves`) | 119.0 | 89.2% | 6.0 ms | 29 | 15 (8) | 33 (3) | nothing |
  | MW2, Rust with bots | 119.0 | 89.4% | 6.2 ms | 49 | 22 (15) | 34 (8) | nothing |
  | MW3, Black Tuesday (27 s) | 118.4 | 89.9% | 5.9 ms | 39 | 23 (20) | 35 (6) | nothing |
  | MW3, Scorched Earth (`sp_berlin`) | 119.1 | 89.3% | 4.7 ms | 20 | 14 (9) | 24 (4) | nothing |
  | MW3, Mind the Gap | 119.1 | 89.0% | 2.9 ms | under 5 | 5 | 19 (1) | nothing |
  | MW3, Seatown with bots | 118.9 | 89.4% | 4.7 ms | 40 | 20 (20) | 41 | nothing |
  | CoD4, Crew Expendable | 119.1 | 89.6% | 3.4 ms | 19 | 11 (3) | 32 (1) | nothing |
  | CoD4, Charlie Don't Surf | 116.7 | 83.0% | 4.5 ms | 38 | 15 (15) | 40 (5) | 10 s at 89 to 110 with the GPU at 4 to 5 ms: the title (one of its threads at 45) |
  | CoD4, The Bog (`bog_b`) | 118.7 | 89.4% | 4.2 ms | 29 | 14 (9) | 43 (3) | nothing |
  | CoD4, Crash with bots | 119.0 | 88.9% | 2.9 ms | 18 | 10 (2) | 31 (1) | nothing |
  | AW, Induction (`seoul`) | 119.1 | 89.1% | 3.6 ms | 16 | 11 (2) | 28 (2) | nothing |
  | AW, Traffic (`lagos`) | 119.0 | 89.1% | 6.6 ms | 48 | 26 (22) | 50 (7) | nothing; five of the title's threads at 39 to 42 each |
  | AW, a match with bots | 119.0 | 89.6% | 3.8 ms | 27 | under 18 (4) | 39 (3) | nothing |

  Advanced Warfare's Fusion, measured apart over seven minutes, is 116.1 with 80.7% on time, held by the title's own threads ([AW-BRINGUP.md](AW-BRINGUP.md)). No scene waits for the recorder thread, and none for the GPU alone: the ring consumer waited 0% of its time for either in every run. In Team Player's views and the Gulag's heavy seconds the GPU's 10 ms would hold the frame at about 100 if the consumer were out of the way; of those 10 ms the scene's pixels are about 1.5 (a 512x300 scene: 9.2 ms), the sun's shadows 2.3 (without them 7.96 ms and 97.4 frames a second), copies, resolves and clears 1.2, and the rest is the scene's 2.6 million indices a frame. The clocks were never held back: the prime core at 3187 MHz and the three performance cores at 2707 to 2803 in every sample (their cap went from 2803 to 2707 once the board passed 60 C and stayed), the GPU between 615 and 680 MHz under a cap of 680. Over ten minutes at Team Player's first view the hottest processor zone stayed at 88 to 91 C, the GPU at 73 to 78 C and the board at 60 to 65 C, and the frame rate went from 80.1 in the second minute to 77.0 in the last four (the GPU's time a frame 10.3 to 10.6 ms); ten minutes of a Seatown match held 119.9 to 120.0 each minute at 88 to 92 C, and ten of Charlie Don't Surf 120.0 after its first minute.
- **Drivers at 120** (Team Player's three views, 50 s, frames a second and GPU a frame; two or three runs each after one that compiled the pipelines). None is faster than the bundled one, so it stays.

  | driver | frames a second | GPU a frame | recorder thread |
  |---|---|---|---|
  | Turnip v27.0.0-R2 (Mesa 27.0.0-devel `2648b2059f`), bundled | 77.8 to 80.2 | 10.1 to 10.3 ms | 27 to 28 |
  | Mesa 26.3.1 (`fec8830e9c`), the latest release, built here | 77.7 to 77.9 | 10.4 ms | 27 |
  | Mesa main (`6c1030248a`, 24 November 2026), built here | 79.1 to 80.2 | 10.0 to 10.2 ms | 27 |
  | the same with `enable_tp_ubwc_flag_hint` for the 740 | 78.8 to 79.1 | 10.2 to 10.4 ms | 27 to 28 |
  | Qualcomm's own (v676, Vulkan 1.3), through render passes | 37.4 | 26.9 ms | 19 to 20 |

  Mesa main against the bundled one elsewhere: Advanced Warfare's Traffic 119.1 and 118.1 against 119.1 and 119.1 (6.6 to 6.7 ms of GPU both), Modern Warfare 3's Scorched Earth 119.1 both (4.5 to 4.6 ms). The builds were made with the NDK's clang for `armv8.2-a` (a build for `cortex-a715` stopped on an instruction this kernel does not allow), the KGSL backend, release. Turnip's own switches on the bundled driver, against 78.0 and 10.3 ms: `TU_DEBUG=noubwc` 72.7 and 11.6 ms (image compression is on for the 740 and is worth 1.3 ms; it never reset this device), `gmem` 67.8 and 14.7 ms, `nolrz` 77.7, `hiprio` 78.3, `TU_AUTOTUNE_ALGO=prefer_sysmem` 77.2, `profiled` 78.6. BC textures are sampled by the chip under Turnip already ([textures.md](textures.md)).
- **The renderer's other ways, measured there and left as they are** (the same views, against 78.0 frames a second and 10.3 ms): graphics pipeline libraries on the bundled Turnip (`MW2_FORCE_PIPELINE_LIBRARIES=1`) 77.8 and 10.4 ms once the cache is full, 15 s of shader translation at a draw on the first visit; render passes in place of dynamic rendering (`MW2_FORCE_LEGACY_RENDERING=1`) 76.8 and 11.0 ms, the recorder thread at 36% of a core for 27; a rendering instance for every segment (`MW2_JOIN_SEGMENTS=0`) 77.0 and 10.9 ms, the recorder at 36; the console's multisampling (`MW2_NO_MSAA=0`) 76.3 and 10.6 ms. Descriptor buffers and push descriptors were not built: every texture is in one set that stays bound and a draw's record is in the frame arena, so a draw writes no descriptor (none shows in the ring consumer's or the recorder's profile). Nor were pixel shaders of lower precision: the pixels are 1.5 ms of the 10.
- **The ring consumer's copies on a second thread, built and not kept** (25 November 2026, `61ca59d` with the game code of `999bd0b`, the same three views; on that build they run at 92 to 95, 84 to 85 and 82 to 84 frames a second, three runs). By `simpleperf` the consumer's 12 ms a frame there are the draws' indices encoded into the index stream (15%: 13 to 15 MB of them a frame), vertex windows copied into the arena (14%: 14 to 16 MB a frame in these views that no slot keeps, 82% of it in pages the title goes on writing -- its soldiers' skinned vertices -- and the rest written since it was kept), the textures' lookups at each of the frame's 55 to 60 segments (9%), and the parse and the draws' own state. The two copies have nothing to decide, so a thread of the performance cores was given them, the consumer making what was still queued itself before each submission and each point where the title is told draws are done (74 a frame), with a table of each index buffer's lowest and highest index under the write watch so the consumer read no index. The views fell to 88 to 91, 82 and 77 to 79 (three runs): the consumer was at work 89 to 92% of the time for 96, but the copier stood ready with no core 43 to 48% of the time and the title's own threads a few points longer than before; the four performance cores are full (the title's main thread and five more of its own, and the recorder), and the three efficiency cores are a quarter as fast. A draw's vertex window shared with the draws after it up to the next such point saved 1 MB of the 9.3 copied a frame over the run and nothing that could be told from the spread between runs (81.3 and 79.8 against 79.8 and 78.8 frames a second over the run), so the copies stay each draw's own.
- **One frame handed over at each blank, measured and left for whoever asks** (`MW2_PRESENT_AT_BLANK=1`). Where the display paces the title the presenter gives the swapchain every frame as it comes and waits where its queue is full; a full queue is a frame of delay, and a frame of room. With the switch it hands over one frame for each blank the system's Choreographer announces (`runtime/android/blank.cpp`), so the queue stays empty while every frame comes inside its blank. By `dumpsys SurfaceFlinger --latency` over 125 frames at a time, three times in each run of 100 to 160 s, a frame was on the screen so many milliseconds after the chip had finished it:

  | scene, limit 120 | as they come: frames a second, seconds under 115, to the screen | one a blank: frames a second, seconds under 115, to the screen |
  |---|---|---|
  | MW2, Just Like Old Times | 119.0, 1, 33.0 to 33.2 ms | 119.0, 0, 23.5 ms |
  | MW3, Black Tuesday | 118.4, 5, 26.2 to 33.3 ms | 118.5, 2, 17.1 to 23.6 ms |
  | MW3, Mind the Gap | 119.0, 0, 32.9 to 33.4 ms | 118.7, 0, 23.5 to 23.6 ms |
  | CoD4, Crew Expendable | 119.1, 0, 33.2 ms | 119.0, 0, 23.5 then 33.0 and 33.1 ms |
  | CoD4, The Bog | | 118.8, 0, 31.9 ms |
  | AW, a match with bots | 119.1, 0, 32.8 ms | 119.1, 0, 23.5 ms |
  | AW, Traffic, three runs of each in turn | 119.1, 119.1 and 119.0; 0, 0 and 0; 32.6 to 33.3 ms | 117.5, 118.1 and 118.9; 12, 8 and 1; 23.5 then 31.8 to 32.0 ms in two of the three |

  Ten milliseconds sooner where the scene is light, a frame at every blank either way. Where some frames take longer than a blank -- Traffic, the renderer's thread at 7.5 of the 8.3 ms -- the full queue is what shows a frame at the blank such a frame misses, and without it each is a blank with no new frame: 21 to 32 stutters in Traffic as they come, 33 to 96 one a blank. A frame handed over late also stays in the queue behind the next, so the delay is the old one again until another frame is late (Crew Expendable, The Bog, Traffic); a blank skipped on purpose to empty it was built and cost more stutters than it saved (48 for 8 in Traffic). What is to be held here is the frame rate, so the switch is off unless asked for, on every chip. The MAILBOX present mode (`MW2_PRESENT_MODE=mailbox`) measures 23.5 to 25 ms as well, but leaves the title drawing 122.4 frames a second for the 120 the screen shows.
- **Before and after, on the merged build** (26 November 2026, the bundled Turnip, caches full, 100 to 160 s of play each; before is the same build with `MW2_SHADOWS=1`). Frames a second, frames on time, seconds under 115 of those played, stutters:

  | scene, limit 120 | before | after |
  |---|---|---|
  | MW2, Team Player's three heavy views (frames a second in each, GPU a frame) | 90.3, 82.4, 80.4 (9.6 ms) | 111.5, 102.1, 101.8 (7.7 ms) |
  | MW2, S.S.D.D. | 116.0, 81.1%, 39 of 162, 106 | 119.0, 93.7%, 0 of 161, 12 |
  | MW2, The Gulag | 110.6, 71.1%, 63 of 160, 732 | 116.9, 86.5%, 17 of 160, 105 |
  | MW2, Just Like Old Times | 119.0, 94.5%, 1 of 160, 4 | 119.1, 94.6%, 0 of 160, 3 |
  | MW2, Rust with bots | 119.1, 94.3%, 0 of 163, 3 | 119.1, 94.1%, 0 of 162, 5 |
  | MW3, Black Tuesday | 118.4, 91.6%, 5 of 100, 37 | the same code |
  | MW3, Scorched Earth | 115.5, 84.0%, 20 of 103, 143 | the same code |
  | MW3, Mind the Gap | 119.0, 93.3%, 0 of 99, 0 | the same code |
  | MW3, Seatown with bots | 118.9, 94.8%, 2 of 165, 53 | the same code |
  | CoD4, Crew Expendable | 119.1, 93.7%, 0 of 127, 3 | the same code |
  | CoD4, Charlie Don't Surf | 115.2, 88.3%, 13 of 121, 68 | the same code |
  | AW, Traffic | 119.0, 95.1%, 0 of 97, 21 | the same code |
  | AW, a match with bots | 119.1, 94.4%, 0 of 216, 22 | the same code |

  Long runs on the branch's builds, the clocks and temperatures read every three seconds: Modern Warfare 2 at Team Player's three views for 26 minutes ran at 92.5 frames a second in the first five and 87.6 in the last (77.8 and 72.0 with the shadows, the day before), Modern Warfare 3's Scorched Earth for 24 minutes of play, started again every 400 s, at 117.9 (118.2 to 119.8 by five minutes), Advanced Warfare's Traffic for 30 minutes at 117.0 (119.6 in the first five, 116.6 to 117.1 after). In all three the prime core's limit stayed 3187 MHz, the performance cores' 2707 and the GPU's 680 (its clock 615 to 666), the hottest core read 90 to 93 C, the GPU 75 to 81 C and the board 63 to 67 C; the device did not restart, the kernel logged no GPU fault and no run lost its Vulkan device. The first and the last of the three ran with `MW2_PRESENT_AT_BLANK=1`. Call of Duty 4 was not run that long.

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

The app does not bundle game files. The player provides an Xbox 360 disc image (`.iso`) of their own disc.

### The guided setup (`Setup.kt`, `SetupActivity.kt`)

A new player is taken from the first start to a game by one button. Until the
game can be played the launcher's large button is **SET UP THE GAME** (then
**CONTINUE SETUP**, or **BUILDING THE GAME · 43%** while a build runs), and the
setup opens by itself once for each start of the launcher until it has been
through. It is numbered screens, each one sentence and one large button:

1. **Pick your disc image.** An image of this game on the device or a memory
   card is offered by name (`DiscImage.search`, where the app may list
   folders); **CHOOSE THE .ISO FILE** is the system's picker, which needs no
   permission, and **Look in a folder** the app's browser. The image is told
   by what is in it before anything else is done (`DiscImage.inspect`, the
   title number in its executable): "This is Advanced Warfare. This app is
   for Modern Warfare 3.", an image with the multiplayer cut out, a version
   the app has no tables for, a document that is no file on the device. The
   game is then set up to play from the image where it lies, which copies
   the two executables and takes a second (the runtime's installer).
   Advanced Warfare's second disc is taken from beside the first, or asked
   for in the same step.
2. **Build the game.** `GameBuildService` builds both titles' game code
   ([ondevice-build.md](ondevice-build.md)); the screen shows one bar
   (`Setup.percent`: the download, the preparing, the compiling, the
   linking), the minutes left, and what Android asks about the toolchain
   package in the order it asks it. Leaving the app leaves the notification
   with the same line, and a tap on it comes back to where the player was. A
   failure is one sentence (no room, no network, the build tools refused, or
   "The build stopped"), **RETRY** and **Share log** under it.
3. **Optional: add your DLC**, once, with **CONTINUE WITHOUT DLC**.

Then **PLAY**. Nothing of this is kept apart from the facts on the device
(the executables in the game folder, the images readable, the game code this
version runs: `Setup.step`), so a setup left half-way, ended by the system or
failed is at the same step the next time, and an app updated over an older
one with the game set up opens on the launcher as before. Measured from a
fresh install on the Retroid Pocket Flip 2 (26 November 2026, an image in
Download, the toolchain package not installed): Modern Warfare 2 from the
first screen to Play in about 8 minutes, 5 min 14 s of it the build, with
three questions of Android's on the way (notifications, "install unknown
apps", "install this app"); Call of Duty 4's build 3 min 49 s.

The launcher after setup is **PLAY**, a line of status ("Ready - campaign
and multiplayer installed") and three groups: *Play options* (Multiplayer,
Bots, and Exo Zombies or the map packs' Special Ops where there are any),
*Extras* (Add DLC, Mods) and *Help and settings* (Graphics driver, Settings,
Log, Report a problem, Check for updates, Repair / rebuild, Quit). **Repair /
rebuild** leads to the two screens the setup runs for the player
(`InstallActivity.kt`: another image, the files copied into the app, a folder
of extracted files; `BuildActivity.kt`: the build with its steps, pausing).
The apps of one title (the older `campaign`, `multiplayer` and `mw3mp`
flavours) keep the install screen on the large button.

The first run of a game shows one screen over it (`GameHelp.kt`): Menu at
the top right or Select and Start together, where the settings are, how to
quit; **OK** or A closes it, **Don't show again** or Y for good.

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
| **"Nothing can be shown"** or **"No picture yet"** over a black screen | The renderer did not come up on this chip and driver, or no frame of the game's reached the screen in 30 seconds ([a run that shows nothing](#a-run-that-shows-nothing)). | The message names the chip and the driver and offers the other driver (the bundled Turnip, or the phone's own) with one tap, which starts the run again; **Share log** sends the log. |
| **"The runtime library would not load"** | Incompatible ABI or 32-bit device. | Ensure device supports 64-bit ARM (`arm64-v8a`). |
| **Installation fails partway** | Insufficient storage space. | Ensure at least 8 GB of free storage is available on internal storage. |
| **"The game is not built yet"**, or "The game could not start: the game code is not on this device" in an older app | The app is published without the game's code, which is built from the player's disc ([game-library.md](game-library.md)). | The launcher's large button builds it on the device; `setup\setup.cmd` on a PC with the device connected over USB is the other way. The log says `app: the run was not started: ...` and `game code: ... is not installed`. |
| **Log inspection** | Debugging game launch or crashes. | The in-app Log viewer's **Share**, or `/sdcard/Android/data/com.mw2.recomp.sp/files/game.log` via ADB. |

### A run that shows nothing

A player on a device nobody here has reported "only a black screen", and the
app had nothing to say about it: when the window or the device could not be
made the presenter logged a line, kept a status nobody read (the dialog that
was to show it waited for a status the runtime never sent) and the game ran
on without a picture. Now (`GameActivity.watchForBlackScreen`, asked twice a
second from the start of a run until the title's first frame is on screen):

- **The runtime knows why** (`android::RendererFailed`, kept for
  `nativeRendererFailure`): no Vulkan driver opened, `vkCreateInstance`
  failed, no surface, no device that presents, a device refused for
  something the renderer needs -- named: "This graphics chip or its driver
  lacks descriptor indexing, which the game's renderer needs", from the same
  test that writes the log's `device: ...; refused: it lacks ...` line
  ([what the renderer asks of a device](#what-the-renderer-asks-of-a-device))
  -- no swapchain, or the renderer itself not starting on a window that did.
  The message is **Nothing can be shown**, with Quit.
- **Nothing says anything** and no frame of the title's is presented within
  30 seconds (ten minutes with *Precompile all shaders* on, which is minutes
  of black by request). The window's own first present is its background
  colour, so the title's first frame is the second present counted. The
  message is **No picture yet**, with Keep waiting.

Either message names the graphics chip (the launcher's OpenGL ES strings,
`DeviceInfo.gpu`, which do not need the Vulkan driver that may be the
trouble) and the driver in use, says what to try, and has two more buttons:
**Use the phone's driver** or **Use Turnip** -- whichever is not in use,
where the app carries one; chosen as Settings' Graphics driver chooses it,
and the run starts again through `HandOffActivity`, since a process opens
its driver once -- and **Share log**, which sends what the launcher's Log
screen shares, as a file (`LogShare`), and leaves the message up.

Pressed on the Retroid Pocket Flip 2, 26 November 2026 (Modern Warfare 2, a
`.dev` build with the two switches below). **Share log** on either message
and the menu's **Share performance report** each opened the system's share
list with `mw2-log-0.1.7-<date>.txt` written for it (34 KB, 65 KB and 3 KB),
and the message was still up behind it. **Use the phone's driver** saved
the choice (`driverDirectory` empty, `driverChoiceMade` true), the run
started again in another process on `the system's libvulkan.so`, and so did
the next start with nothing forced.

Seen on the AYN Thor with the two switches that stand in for such a device
in a build with diagnostics: `MW2_TEST_REFUSE_DEVICE=1` (the device refused
as one without descriptor indexing: the message was up 20 seconds into the
run, the first time looked) and `MW2_TEST_NO_FRAMES=1` (every frame of the
title's withheld from the window: the message at 30 seconds). On the Flip 2
the message over no picture came 32 seconds after the start; its **Use
Turnip** is the one button not pressed on a device (the tap meant for it
landed on Keep waiting).

### What the log share holds

**Share** on the Log screen offers the log as a file,
`<game>-log-<version>-<date>.txt`, through the system's share list
(`LogShare.kt`: a FileProvider over the app's cache), or the end of it on the
clipboard for a chat. The file is what a bug report's form takes as an
attachment; as text it was more than an issue holds (65,536 characters).
**Report a problem** opens that form with the game, the app's version and the
device filled in. The text is enough for a bug report (`LogSummary.kt`,
`DeviceInfo.kt`), 200,000 characters at most:

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
