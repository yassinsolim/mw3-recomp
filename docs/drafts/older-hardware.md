# Older phones: drivers, a renderer fallback, and what the hardware can do

Draft, 27 November 2026. Research only: nothing here changes the app, the runtime or the `android` branch. Everything marked **measured** or **checked** was run on the PC (Windows, WSL Ubuntu 24.04, NDK r28c); nothing was run on a phone. Claims about the outside world link their source; claims about our code name the file.

## The short version

1. **The Realme 5 Pro's crash is ours, not Turnip's.** `libmw3.so` exports libadrenotools' own globals (`android_get_exported_namespace`, `android_link_namespaces`, `android_link_namespaces_all_libs`) as data symbols with the names of bionic's linker functions. On the player's phone a library loaded under the imported driver calls `android_get_exported_namespace()` as a function, gets our 8-byte variable instead, and jumps into `.bss`. The faulting PC in his log is exactly that variable in the 0.1.12 build (worked out below). Every custom driver he imports dies the same way, and a driver we build ourselves would too. **The fix is a link flag and takes hours.**
2. **The bundled Turnip was never offered on his phone because of the Android version, not the chip.** `DriverStore.isQualcomm()` needs `Build.SOC_MANUFACTURER`, which exists from Android 12 (API 31) on. Every Snapdragon on Android 10 or 11 is treated as "not Qualcomm". The bundled Turnip v27.0.0-R2 does list the Adreno 616.
3. **Building Turnip ourselves works** (checked: Mesa main `483aff363be`, 690 build steps, one `libvulkan_freedreno.so`). Built against API 29 it imports nothing newer than `LIBC_P`. Every community Turnip build I checked, the bundled one included, needs `LIBC_R`, so **none of them can load on Android 10**, whatever their `meta.json` says. Upstream Turnip lists the Adreno 615/616/618/619 and turns descriptor indexing on for every Adreno 6xx without conditions.
4. **Mali:** a community PanVK that runs on the stock `mali_kbase` kernel driver now exists, packaged for adrenotools. Our runtime would load it on a Mali today. For reach it changes nothing: Valhall phones (G57/G68/G77/G78) already pass on Arm's own driver, and on Bifrost (G52/G76) PanVK has no update-after-bind either, because the hardware's sampler count field is 16 bits. For the Retroid Pocket 4 Pro's black screen it is worth a try.
5. **A renderer without descriptor indexing** would open about 1,100 more device models on gpuinfo.org: Mali Bifrost, PowerVR Rogue, Adreno 5xx, and Adreno 6xx on 2020-era drivers. Almost all of them pair the GPU with two Cortex-A75/A76 cores or only A53s. It means two renderers to keep, and it brings back the per-draw descriptor set work that cost the recorder 2.9 times more cycles per draw. **Not recommended now.**
6. **What to promise:** Snapdragon 720G/730/732G and Helio G99 have about 60% of a Snapdragon 865's single-core speed, half its big cores, and 25 to 30% of its GPU. Expect **30 fps in most places with a smaller scene**. Expect less than 30 in the heaviest views. A Snapdragon 710/712 (36% of the CPU, 20% of the GPU) is a **20-30 fps device at best**.

### Recommendation, ranked

| # | What | Effort | Who it reaches | Risk |
|---|---|---|---|---|
| 1 | Hide libadrenotools' symbols in `libmw3.so`/`libmw3mp.so` (`-Wl,--exclude-libs,libadrenotools.a`, or a version script) | hours, plus one test on an Android 11 Adreno phone | every Android 10/11 Snapdragon player who imports a driver (the Realme 5 Pro first) | small; the crash handler's `sub_XXXXXXXX` names stay exported |
| 2 | Offer the bundled Turnip on Android 10-11 Snapdragons: detect Qualcomm without `SOC_MANUFACTURER` (the runtime's own `AdrenoHardware()` reads `/sys/class/kgsl/kgsl-3d0/gpu_model`), and bundle a Turnip built at API 29 (the script below) instead of the community `LIBC_R` build | 2-4 days, including a reproducible build in `fetch_deps.sh`/CI and the MIT notice | Snapdragon 662/665/670/675/710/712/720G/730/732G phones (Adreno 610-619) still on their 2019-21 stock drivers (Vulkan 1.1.128, no descriptor indexing): the gpuinfo reports show 512.502/512.530 drivers refused and Turnip on the same chips passing. For Adreno 620 and up, 780 of 782 device models on gpuinfo already report descriptor indexing in their newest report | Turnip on a6xx gen1 has had less testing than on a7xx. Needs one real device in the 615-619 class before it is the default |
| 3 | Tell players what to import today (list below) | a docs/FAQ change | the same phones, once #1 has shipped | proprietary repacks are untested and some break the phone's UI (their own notes say so) |
| 4 | Ask the Retroid Pocket 4 Pro player to try faux123's PanVK | one message | one player (Mali-G77, r32p1) | experimental driver; its author has tested one game |
| 5 | Per-draw descriptor renderer fallback | 3-5 weeks, then forever | Helio G80-G96 (Mali-G52/G76), Exynos 9611/9820, Kirin 980/990, Snapdragon 660/636 (Adreno 5xx), Helio G35/P35 (PowerVR) | slow on exactly the CPUs that can least afford it; a second renderer in every future change |

## 1. Turnip for older Adreno

### Which chips current Turnip supports

Mesa main (`483aff363be`, 27 November 2026), [`src/freedreno/common/freedreno_devices.py`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/483aff363be/src/freedreno/common/freedreno_devices.py):

| Group in the table | Chips | Notes |
|---|---|---|
| a6xx gen1 low | 605, 608, 610, 612, 613 | `reg_size_vec4 = 32/48`, 128 KB GMEM, no fast clear, no sampler min/max |
| **a6xx gen1** | **615, 616, 618, 619** | 512 KB GMEM, one CCU, `blit_wfi_quirk` |
| a6xx gen2-4 | 620, 621, 623, 630, 640, 643/7c+ Gen 3, 644, 650, 660, 663, 680, 690 | |
| a7xx, a8xx | 702 ... 750, 810 ... 840 (the 830's KGSL entry `0x44050001` is in the 26.3.1 release too) | a8xx fixed through November 2026 (docs/android.md, Adreno 8xx) |

**Descriptor indexing is on for every one of them.** [`tu_device.cc`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/483aff363be/src/freedreno/vulkan/tu_device.cc#L543) sets `descriptorIndexing`, `shaderSampledImageArrayNonUniformIndexing`, `descriptorBindingSampledImageUpdateAfterBind`, `descriptorBindingPartiallyBound` and `runtimeDescriptorArray` to `true` with no chip test. The limits are `MAX_SET_SIZE / (4 * FDL6_TEX_CONST_DWORDS)` = 2^24 descriptors, well above the 65,536 images the renderer asks for (`tools/check_device_report.py`, `IMAGE_COUNT`).

gpuinfo.org agrees. Checked with `tools/check_device_report.py`:

| Report | GPU, driver, Android | Verdict |
|---|---|---|
| [14753](https://vulkan.gpuinfo.org/displayreport.php?id=14753) | Adreno 616, Qualcomm 512.502 (the player's own phone model, RMX1971), Android 11 | **refused**: no descriptor indexing |
| [22915](https://vulkan.gpuinfo.org/displayreport.php?id=22915) | Adreno 618, Qualcomm 512.530, Android 13 | **refused**: no descriptor indexing |
| [33972](https://vulkan.gpuinfo.org/displayreport.php?id=33972) | Adreno 616, Qualcomm 512.786 (Realme XT, Android 14) | **runs**: render passes, BC in software, a record per pixel, 8 fallbacks |
| [27310](https://vulkan.gpuinfo.org/displayreport.php?id=27310) | Adreno 618, Qualcomm 512.744.12, Android 14 | **runs** |
| [31966](https://vulkan.gpuinfo.org/displayreport.php?id=31966) | Adreno 616, **Turnip** (Mesa 24.2-devel), Android 13 | **runs**: dynamic rendering, Vulkan 1.3, 5 fallbacks |

So the hardware can do it. Qualcomm's own drivers added descriptor indexing on a6xx between 512.530 and 512.611 (gpuinfo has an Adreno 618 on 512.611 with the extension). Phones that never got an update past Android 11 are stuck on 512.502/512.530.

### Building it ourselves: checked

Built with the script in [the appendix](#appendix-a-the-turnip-build): Mesa main `483aff363be` in its own worktree, NDK r28c's `aarch64-linux-android29-clang`, `-Dplatforms=android -Dplatform-sdk-version=29 -Dandroid-stub=true -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl`. Result:

- 690 build steps, all passing. About a minute on this PC, but with a ccache warmed by earlier Mesa builds in the same WSL, so that is not a cold build time.
- `libvulkan_freedreno.so` is 13.1 MB stripped. Its NEEDED entries are `libhardware liblog libnativewindow libsync libm libz libdl libc`. Its newest symbol versions are `LIBC_N` and `LIBC_P`, so **it loads from Android 9 up**.
- It still carries the Adreno 616/618 device entries.
- Packaged with a `meta.json` in the DriverStore layout (`minApi 29`). The zip passes the same checks `DriverStore.import` makes (meta.json, `libraryName` present, ELF AArch64). The zip's SHA-256 is `d7b3bdd1…7051`. It is not published.

#### (a) Loading on Android 10-11

Two separate problems:

**Android 10: `LIBC_R`.** C11 `<threads.h>` (`call_once`, `mtx_*`, `cnd_*`, `thrd_*`) entered bionic in API 30 ([bionic status](https://android.googlesource.com/platform/bionic/+/main/docs/status.md), "New libc functions in R"). A Turnip compiled with an API 30+ clang imports them as `@LIBC_R`, and Android 10's linker refuses the library. Compiled at API 29, the NDK's inline versions are used instead. That is the only difference between our build and theirs here, and it decides Android 10:

| Package | Mesa | `meta.json` minApi | newest libc version needed | Android 10? |
|---|---|---|---|---|
| bundled `turnip_v27_r2.zip` ([StevenMXZ v39](https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/tag/v39)) | 27.0-devel `2648b2059f` | 28 | `LIBC_R` | **no** |
| Turnip v26.0.0 R8 (the player's, [K11MCH1](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/tag/v26.0.0-rc08)) | 26.0-devel `5ac41be677` | 27 | `LIBC_R` | **no** |
| **ours** | 27.0-devel `483aff363b` | 29 | `LIBC_P` | **yes** |

The rest are in [the survey](#4-drivers-that-already-exist). All of them claim a minApi they cannot meet, and `DriverStore` trusts `minApi`. An Android 10 player gets a driver that imports fine and then fails in `dlopen`.

**Android 11: the crash at `vkCreateInstance` is our symbol collision.** The player's log (`mw3-log-0.1.12-20261128-0347.txt`) shows `SIGSEGV`, `SEGV_ACCERR`, with the PC at `0x6ef1724a80` inside the `[anon:.bss]` mapping `6ef1156000-6ef1727000`, on the thread `vulkan presenter`. `x17` equals the PC and `x16` points at a GOT slot, so the code reached it by calling a function through a PLT entry. The caller is a small library at `0x6edf…`, not Turnip (no slot in Turnip's `.got.plt` ends in `…980`).

Mapping the address onto the 0.1.12 release's `libmw3.so` (`out/release/drafts/mw3-v0.1.12/mw3-recomp-v0.1.12.apk`):

- `.data` ends at `0x6107f8`, so the anonymous `.bss` pages start at `0x611000`. The log's mapping is `0x5d1000` long, so it ends at `0xbe2000`, which matches `.bss` (`0x610800 + 0x5d0b10`) rounded up to a page.
- The load base is therefore `0x6ef1727000 - 0xbe2000 = 0x6ef0b45000`. That is consistent with the stack's `<=` return addresses, which fall inside `libmw3.so`'s code.
- The fault offset is `0x6ef1724a80 - 0x6ef0b45000 = 0xbdfa80`.
- `0xbdfa80` in `libmw3.so`'s dynamic symbol table: `OBJECT GLOBAL DEFAULT .bss android_get_exported_namespace`.

`libmw3.so` links libadrenotools statically (`cmake/android.cmake`, `target_link_libraries(mw2 PRIVATE adrenotools)`). Its `linkernsbypass.cpp` keeps function pointers in globals named `android_get_exported_namespace`, `android_link_namespaces` and `android_link_namespaces_all_libs`. Those are the names of real functions in bionic's `libdl_android`. Because `cmake/android.cmake` keeps default visibility (on purpose, for the crash handler's `sub_XXXXXXXX` names), they land in `libmw3.so`'s dynamic symbol table, and a library that resolves the function by name can bind to our variable instead. On this phone one does, during instance creation: Android 11's `libvndksupport` asks for exactly that function [INFERENCE: the caller is not named in the log]. `libmw3mp.so` exports the same three, and the Turnip used makes no difference. **A driver we build would crash the same way until this is fixed.**

Fix (for whoever owns the runtime): link `mw2`/`mw2mp` with `-Wl,--exclude-libs,libadrenotools.a` (lld honours it per archive), so only the static library's symbols are hidden. Same class of hazard, lower risk: `libmw3.so` also exports 116 `vk*` function-pointer globals as `OBJECT`s (`vkCreateSampler`, `vkCmdEndRenderPass`, ...). No library is known to bind to them today.

Not KGSL: `vkCreateInstance` in Turnip opens no device. KGSL is first touched at `vkEnumeratePhysicalDevices` ([`tu_knl_kgsl.cc`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/483aff363be/src/freedreno/vulkan/tu_knl_kgsl.cc)), so a KGSL version mismatch would show up at the next step, `vulkan-surface`. The R8 package also needs `LIBC_R` (fine on Android 11) and `libcutils`.

#### (b) Descriptor indexing on a6xx gen1

Yes, unconditionally (above). A Turnip on the Adreno 616 also gives Vulkan 1.3 with dynamic rendering, which the stock 512.786 does not (report 31966 against 33972).

#### (c) Tuning for our workload

Nothing needs to be built in. The runtime already sets `TU_DEBUG=noubwc` on every Adreno 6xx before the driver reads it (`runtime/android/driver.cpp`, `KeepTurnipOffUbwcOnAdreno6`). `FD_DEV_FEATURES` reaches the same code at run time. Worth doing in our own build:

- build for API 29 (the point above);
- pin the Mesa commit and patch nothing, so a bug can be reported upstream;
- build only `freedreno` with `kgsl` (as here, about 690 steps);
- package with a truthful `minApi`.

Tuning `-mtune` for the A75 makes no measurable difference [INFERENCE, not measured].

## 2. Why the Adreno 616 was not offered the bundled Turnip

`android/app/src/main/java/com/mw2/recomp/DriverStore.kt`:

```kotlin
private fun isQualcomm(): Boolean =
    Build.VERSION.SDK_INT >= 31 && Build.SOC_MANUFACTURER == "QTI"
```

[`Build.SOC_MANUFACTURER`](https://developer.android.com/reference/android/os/Build#SOC_MANUFACTURER) was added in API 31. On the Realme 5 Pro (Android 11, API 30) `installBundled` returns at `if (!isQualcomm()) return`, so the package is never unpacked and never listed. The rule has nothing to do with the chip; every Snapdragon below Android 12 is left out.

Does the bundled v27.0.0-R2 support the 616? Its device table has the 616 and 618 entries (checked with `strings`), so yes, on Android 11. Not on Android 10 (`LIBC_R`), and not on Android 11 either until the symbol collision is fixed.

Suggested rule: use `Build.SOC_MANUFACTURER` where it exists, and below API 31 fall back to the same evidence the runtime uses (`AdrenoHardware()`: the kgsl `gpu_model` node, `ro.hardware.vulkan`, `ro.soc.manufacturer`). Ship an API 29 Turnip so Android 10 is covered too.

## 3. Mali

### Upstream PanVK

From [Mesa's Panfrost page](https://docs.mesa3d.org/drivers/panfrost.html) and the source:

- Vulkan 1.3 for Bifrost (G31/G51/G52/G71/G72/G76), 1.4 for Valhall v10+ (G310/G610 and newer). **Nothing for Valhall v9** (G57, G68, and the G77/G78 aren't even listed). `src/panfrost/vulkan/meson.build` builds arches 6, 7 and 10-14 and skips 9.
- Kernel interface: only `panfrost`, `panthor` and `virtio` (`src/panfrost/lib/kmod/`). Retail Android ships Arm's `mali_kbase`, so upstream PanVK cannot run on a stock phone at all.
- Descriptor indexing: `descriptorBindingSampledImageUpdateAfterBind = PAN_ARCH >= 9`, and the same for partially bound and update-unused-while-pending (`panvk_vX_physical_device.c`). On Bifrost the per-stage sampled image limit is 256, and the hardware's `sampler_count` is a 16-bit field (`panvk_descriptor_set_layout.h`). **PanVK cannot give a Bifrost Mali what our renderer asks for.**

### Out-of-tree PanVK on `mali_kbase`

These are new in autumn 2026, all experimental:

| Project | Version, date | Claims | Package | Fits DriverStore? |
|---|---|---|---|---|
| [zenithblue-oss/panvk-kbase-android](https://github.com/zenithblue-oss/panvk-kbase-android) | [v26.3.1](https://github.com/zenithblue-oss/panvk-kbase-android/releases/tag/panvk-kbase-v26.3.1), 31 Oct 2026 | one `.so` for v6-v14 (Bifrost, Valhall v9, CSF v10-v14); tested on G72, G52, G57, G78, G710, G715, G615, G720, G925, G1-Ultra | `-adrenotools.zip`: `libvulkan_panfrost.so`, minApi 29, needs `LIBC_P` | **yes** (checked). The `.adpkg.zip` says minApi 35 and is refused below Android 15. The `-gamehub.zip` names its library `libvulkan_freedreno.so`, which `TurnipWithoutAdreno` would refuse on a Mali |
| [faux123/panvk-faux123-driver-rp4pro](https://github.com/faux123/panvk-faux123-driver-rp4pro) | v1.1.9, 26 Nov 2026 | built on zenithblue's series plus 49 patches. Tested by its author on **a Retroid Pocket 4 Pro (Mali-G77 MC9, r32p1 kbase, Android 13)**, an RG556 (G57), a Pixel 7 and a GameForce Ace. "One game has been run on it" | `.adpkg.zip`, minApi 29, `LIBC_P` | **yes** (checked) |
| [MineGraph panvk-Android](https://github.com/MineGraph-bilibili/panvk-Android/releases) | panvk.6, 2 Nov 2026 | verified on a G715 only; its notes warn of kernel panics in some D3D11 games ([guide](https://overclockedhq.com/guides/panvk-mali-driver)) | minApi 30 | not downloaded |

**Can an app load it?** Yes. libadrenotools works on Mali; its author said so in [bylaws/libadrenotools#2](https://github.com/bylaws/libadrenotools/issues/2). Dolphin merged Mali support for custom drivers on 2 November 2026 ([dolphin-emu/dolphin#14958](https://github.com/dolphin-emu/dolphin/pull/14958)). Our runtime does not limit adrenotools to Adreno: it only refuses a library whose name says Turnip on a non-Adreno chip (`runtime/android/driver.cpp`, `TurnipWithoutAdreno`). So **a Mali player can import `libvulkan_panfrost.so` in the app today** [INFERENCE: not run on a Mali].

**Is it realistic?** Not for reach:

- Valhall v9 already passes on Arm's driver. The Retroid's r32p1 has descriptor indexing and 500k sampled images per stage (agent MaliG77's check). Helio G99's G57 passes on r32 and r49 (reports [53699](https://vulkan.gpuinfo.org/displayreport.php?id=53699), [53612](https://vulkan.gpuinfo.org/displayreport.php?id=53612)).
- Bifrost stays out on every driver (above).

What PanVK is worth: a second opinion on the Retroid Pocket 4 Pro's black screen, where faux123's build is tested on that exact device and kernel driver. It is not something to bundle.

## 4. Drivers that already exist

Checked 27 November 2026. "Loads in our app" means the zip passes `DriverStore.import`'s checks (checked on the PC by repeating its logic). It also covers whether the library's symbol versions and NEEDED libraries can resolve on the stated Android version (checked with `readelf`). None of it was run on a phone. On Android 10/11 every custom driver is blocked by the symbol collision until fix #1 ships.

### Community Turnip (Mesa, MIT: we could redistribute these with the notice)

| Build | Date, Mesa | Chips it claims | minApi in meta / really needs | a616 entry | Loads in our app |
|---|---|---|---|---|---|
| [StevenMXZ v40, Turnip v27.0.0-R3](https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/tag/v40) | 23 Nov 2026, 27.0-devel `c5ef4bdfab` | "a6xx, a7xx, a8xx … in a single driver" | 28 / **Android 11** (`LIBC_R`) | yes | Android 11+ |
| bundled, StevenMXZ v39 R2 | 7 Nov 2026, `2648b2059f` | a7xx ("A7xx Android/Bionic") | 28 / Android 11 | yes | Android 11+ |
| [K11MCH1 v26.0.0 R8](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/tag/v26.0.0-rc08) | 8 Jan 2026, `5ac41be677` | a6xx/a7xx, "super beta" a8xx | 27 / Android 11 | yes | Android 11+ (the player's) |
| [MrPurple T30](https://github.com/MrPurple666/purple-turnip/releases/tag/vturnip_mrpurple_T30-toasted.adpkg) | 17 Aug 2026, 26.3-devel | "Unified: A6XX, A7XX, A8XX", "Android 11+" | 30 / Android 11 | yes | Android 11+ |
| [WinNative v1.19 (b)](https://github.com/WinNative-Emu/Drivers/releases/tag/v1.19) | 30 Sep 2026, 26.3-devel `30da39187d` | "Axxx" | 29 / Android 11 | yes | Android 11+ |
| [whitebelyash Stable Turnip v2](https://github.com/whitebelyash/AdrenoToolsDrivers/releases/tag/stu_v2) | 19 Sep 2026, Mesa 26.2.3 | stable release | 28 / Android 11 | yes | Android 11+ |
| [The412Banner, daily](https://github.com/The412Banner/Banners-Turnip/releases/tag/v27.0.0-20261127-r4) | 27 Nov 2026, `ba14a35fbc` | automated, "not tested on a device" | 28 / Android 11 | yes | Android 11+ |
| [Weab-chan CI](https://github.com/Weab-chan/freedreno_turnip-CI/releases/tag/25.3.0-devel_bbdd688) | 11 Oct 2025, 25.3-devel | a6xx/a7xx | 27 / Android 11 | yes | Android 11+ |
| **ours (not published)** | 27 Nov 2026, `483aff363b` | upstream table | 29 / **Android 10** (`LIBC_P`) | yes | Android 10+ |

None of them is aimed at a6xx gen1 or at Android 10. All of them carry the 616 entry because upstream does. For the 615-619 class, an Android 11 player can use any of them once fix #1 ships. An Android 10 player can only use one built at API 29.

### Repackaged Qualcomm drivers (proprietary: we cannot redistribute them; a player may import one through Graphics driver → import)

Each package ships the blob with its dependencies renamed (`notgsl.so`, `notllvm-glnext.so`, `notadreno_utils.so`, ...). They all need only `LIBC_Q` (Android 10). They also link against the phone's own `vendor.qti.hardware.display.mapper@2.0/3.0/4.0`, `android.hardware.graphics.mapper@4.0` and `libhidlbase`. A phone whose vendor image lacks mapper 4.0, which is common on devices that launched with Android 9-10, fails to load them [INFERENCE from the NEEDED list].

| Package | Date | Source and the packager's own notes | minApi | a6xx descriptor indexing |
|---|---|---|---|---|
| [v615.77](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/tag/v615.77) | 21 Dec 2023 | K11MCH1 | 27 | likely: gpuinfo has Adreno 618/619 phones on 512.611-512.615 with `VK_EXT_descriptor_indexing` |
| [v676.53](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/tag/v676.53) | 21 Feb 2024 | K11MCH1 | 27 | likely (same) |
| [v744.19](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/tag/v744.19) | 10 Apr 2024 | K11MCH1 | 27 | yes on stock: Adreno 618 512.744.12 runs (report 27310) |
| [v762.10](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/tag/09072024) | 7 Sep 2024 | "extracted from Xiaomi 12. May work on 6xx … will break whole Android user interface while game is running" | 27 | 512.762 on Adreno 619 phones has it |
| [v805](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/tag/v805) | 22 Feb 2025 | "from Meta Quest 3. Works on a6xx and a7xx" | 27 | not on gpuinfo for a6xx |
| [v849_a6xx](https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/tag/v849) | 8 Mar 2026 | StevenMXZ, a6xx build | **32** (refused below Android 12L) | unknown |
| v837/v840 ([K11MCH1](https://github.com/K11MCH1/AdrenoToolsDrivers/releases)) | Jan 2026 | Quest 3 / Ray-Ban, "For a7xx. Maybe a6xx" | not downloaded | unknown |

The 512.786 driver that runs on a Realme XT's 616 (report 33972) is a stock driver from a newer ROM, not a package.

### Mali

See [section 3](#3-mali): zenithblue v26.3.1 (`-adrenotools.zip`) and faux123 v1.1.9 load in our app on paper. There is no community repack of Arm's proprietary Mali driver that an app loads, and Bifrost gets no descriptor indexing from any of them.

### What to tell players today

Everything here is untested in this app unless it says otherwise.

| Phone | Today | After fix #1 |
|---|---|---|
| Snapdragon 662/665/670/675/710/712/720G/730/732G on **Android 12+** | the bundled Turnip is chosen for them | same |
| The same chips on **Android 11** (stock driver 512.4xx-512.530) | nothing works: the stock driver is refused, and every imported driver crashes at start (our bug) | import Turnip v27.0.0-R3 ([StevenMXZ v40](https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/tag/v40)) or K11MCH1 R8. Untested; Turnip on a 616 passes the device check (report 31966). Fallback, untested: Qualcomm v744.19 or v676.53 ([K11MCH1](https://github.com/K11MCH1/AdrenoToolsDrivers/releases)) |
| The same chips on **Android 10** | nothing works | no public Turnip loads (`LIBC_R`). Our API 29 build would, once shipped. Qualcomm v615.77/v676.53 might, if the vendor image has mapper 4.0 |
| Adreno 610/612 (Snapdragon 665/680/460), stock 512.502 | refused | same as the 615-619 rows (Turnip has them), but a weaker GPU still |
| Mali-G57/G68/G77/G78 | the system driver (passes the checks) | same. The RP4 Pro player may try faux123 v1.1.9 for the black screen |
| Mali-G52/G76/G72/G71, PowerVR, Adreno 5xx | refused; no driver helps | only the renderer fallback (section 5) would change this |

## 5. A renderer without descriptor indexing

### Who is turned away now

gpuinfo.org, Android device models in its database, split by whether their newest report has `VK_EXT_descriptor_indexing` ([without](https://vulkan.gpuinfo.org/listdevicescoverage.php?extension=VK_EXT_descriptor_indexing&platform=android&option=not), [with](https://vulkan.gpuinfo.org/listdevicescoverage.php?extension=VK_EXT_descriptor_indexing&platform=android); queried 27 Nov 2026 through the site's device-list API): **1,180 without, 1,994 with**.

| Family | without | with | Note |
|---|---|---|---|
| PowerVR (GE8300/8320/8322, ...) | 293 | 38 | |
| Adreno 5xx | 264 | 2 | |
| Mali-G52 | 232 | 0 | Helio G80-G96, many budget phones |
| Mali-G76 | 88 | 0 | Helio G90T/G95, Exynos 9820, Kirin 980/990 |
| Mali-G72 | 77 | 0 | |
| Mali Midgard (T-series) | 63 | 0 | Vulkan 1.0-1.1 era |
| Mali-G71 | 50 | 0 | |
| Mali-G51 | 35 | 0 | |
| Adreno 605-613 | 13 | 73 | old stock drivers |
| Adreno 615-619 | 9 | 50 | old stock drivers |
| Adreno 620+, 7xx, 8xx | 2 | 780 | |
| Mali Valhall (G57/G68/G77/G78) and newer | 0 | 869 | |

What the limits are on the turned-away families (from reports [53723](https://vulkan.gpuinfo.org/displayreport.php?id=53723) G52 r27, [53608](https://vulkan.gpuinfo.org/displayreport.php?id=53608) G76 r26, [53553](https://vulkan.gpuinfo.org/displayreport.php?id=53553) GE8320, [48388](https://vulkan.gpuinfo.org/displayreport.php?id=48388) Adreno 509):

| | sampled images per stage | samplers per stage | descriptor sets | anything else `check_device_report.py` fails |
|---|---|---|---|---|
| Mali-G52/G76 (r26-r27) | 256 | 128 | 4 | descriptor indexing and non-uniform indexing only |
| PowerVR GE8320 | 48 | 32 | 4 | the same |
| Adreno 509 (512.415) | 128 | **16** | 4 | also refuses R4G4B4A4, R16, R16G16 and R16G16B16A16 UNORM textures |
| Adreno 616 (512.502) | 524,288 | 524,288 | 4 | descriptor indexing only |

A Xbox 360 draw samples at most 16 pixel and 4 vertex textures. A per-draw set of 20 images fits all of these, but Adreno 5xx's 16 samplers is a squeeze and its texture formats are extra work.

### What it would cost

- **Build:** the pre-29-October design (per-draw texture set) was replaced in `7642d6d` (869+/589- lines in 24 files). Since then 69 commits have touched `bindings.h`, `shader_translator.cpp`, `texture_cache.cpp` and `renderer_draw.cpp` assuming the bindless set. These include the shared 65,536-image array, the draw record with a texture handle per slot, subgroup-uniform texture indexing and multi-draw runs. A fallback means:
  - a second shader variant where each slot is a binding, not an index;
  - a descriptor-set allocator and cache keyed by the draw's textures;
  - a recorder that breaks a multi-draw run at every texture change;
  - device selection and tests for both.

  Estimate: **3-5 weeks** for someone who knows the renderer [INFERENCE].
- **Keep:** every renderer change from then on needs testing both ways, on hardware nobody here owns.
- **Performance:** `7642d6d` measured the recorder at 2,835 cycles a draw with per-draw sets and push constants, and 994 without. The fallback brings most of that back. Per-draw sets also break the runs that let 97% of draws go out about thirty to a call. On a phone with two A75s that is several milliseconds of the main bottleneck every frame [INFERENCE from those numbers].

**Who it would reach:** Helio G80-G96 phones (2×A75/A76 at 2.0-2.05 GHz, Mali-G52 MC2 / G76 MC4), Snapdragon 660/636 (Kryo 260, Adreno 512/509, which also fail texture formats), Helio G35/P35 (A53 only, PowerVR GE8320). By section 6 these are the phones least able to run the games. **Not recommended until #1-#2 are done and players on Valhall/Adreno 6xx have shown what mid-range hardware gives.**

## 6. What frame rate to promise

The project measures on a Snapdragon 865 (Retroid Pocket Flip 2). The heaviest places there (`docs/android.md`, "The campaigns' slow places"):

- **renderer thread** 13-18 ms a frame in CoD4's Charlie Don't Surf;
- **GPU** 17-22 ms a frame in MW2's flight to the Gulag at the 832x480 scene the runtime picks for Adreno 6xx;
- otherwise 60 fps.

| SoC | big cores | GB6 single ([cpu-monkey](https://www.cpu-monkey.com/en/cpu-qualcomm_snapdragon_865)) | vs 865 | 3DMark Wild Life Extreme ([nanoreview](https://nanoreview.net/en/soc/qualcomm-snapdragon-865)) | vs 865 |
|---|---|---|---|---|---|
| Snapdragon 865 | 1×A77 2.84 + 3×A77 2.42 | 1176 | 1.00 | 1110 | 1.00 |
| Snapdragon 710 (Adreno 616) | 2×A75 2.2 | [428](https://www.cpu-monkey.com/en/cpu-qualcomm_snapdragon_710) | 0.36 | [228](https://nanoreview.net/en/soc/qualcomm-snapdragon-710) | 0.21 |
| Snapdragon 720G (Adreno 618) | 2×A76 2.3 | [732](https://www.cpu-monkey.com/en/cpu-qualcomm_snapdragon_720g) | 0.62 | [314](https://nanoreview.net/en/soc/qualcomm-snapdragon-720g) | 0.28 |
| Snapdragon 730 (Adreno 618) | 2×A76 2.2 | [706](https://www.cpu-monkey.com/en/cpu-qualcomm_snapdragon_730) | 0.60 | [269](https://nanoreview.net/en/soc/qualcomm-snapdragon-730) | 0.24 |
| Helio G99 (Mali-G57 MC2) | 2×A76 2.2 | [712](https://www.cpu-monkey.com/en/cpu-mediatek_helio_g99) | 0.61 | [346](https://nanoreview.net/en/soc/mediatek-helio-g99) | 0.31 |
| Dimensity 1100 (Mali-G77 MC9, the RP4 Pro) | 4×A78 2.6 | [966](https://www.cpu-monkey.com/en/cpu-mediatek_dimensity_1100) | 0.82 | [1238](https://nanoreview.net/en/soc/mediatek-dimensity-1100) | 1.12 |

Scaling the 865's worst places by those ratios gives rough figures, not measurements:

- **SD720G/730/732G, Helio G99:** renderer thread 21-30 ms and GPU 60-80 ms in the heaviest views at 832x480. At the smallest dynamic scene (58% of the width, about a third of the pixels) the GPU is perhaps 25-35 ms, since not all of the GPU's time scales with pixels. There are also only two big cores, where the 865 gives the ring consumer a core to itself and the title, recorder and workers three more (`docs/android.md`, Threads). **Promise 30 fps, held by `MW2_FPS_STEPS=60,30`.** Light levels and menus may reach 60; the heaviest views will dip below 30.
- **SD710/712:** renderer thread 36-50 ms, GPU about 5× the 865 in the heaviest views. **20-30 fps, and under 20 in the heaviest views.** Playable for campaign menus and light levels, not something to advertise.
- **Dimensity 1100:** CPU a little under the 865 and GPU about equal, so 60 is plausible once the black screen is understood.
- RAM: most of these phones have 4-6 GB. The runtime already sizes its caches by memory (`perf.cpp`); not studied here.

## Appendix A: the Turnip build

Run in WSL Ubuntu 24.04 with Meson 1.12, the NDK r28c at `/opt/android-sdk/ndk/28.2.13676358`, and an existing Mesa clone at `~/mesa-tg/mesa` (fetched to `483aff363be`):

```bash
NDK=/opt/android-sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin
W=~/olderhw; mkdir -p $W/empty-pkgconfig
git -C ~/mesa-tg/mesa worktree add --detach $W/mesa FETCH_HEAD
cat > $W/android29-aarch64.txt <<EOF
[binaries]
ar = '$NDK/llvm-ar'
c = ['ccache', '$NDK/aarch64-linux-android29-clang']
cpp = ['ccache', '$NDK/aarch64-linux-android29-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$NDK/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$W/empty-pkgconfig', '/usr/bin/pkg-config']
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8.2-a'
endian = 'little'
EOF
cd $W/mesa
meson setup $W/build --cross-file $W/android29-aarch64.txt \
  -Dbuildtype=release -Dplatforms=android -Dplatform-sdk-version=29 -Dandroid-stub=true \
  -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dvulkan-beta=true -Dfreedreno-kmds=kgsl \
  -Dstrip=true -Degl=disabled -Dandroid-libbacktrace=disabled \
  -Dc_args='-march=armv8.2-a -mtune=cortex-a75' -Dcpp_args='-march=armv8.2-a -mtune=cortex-a75'
ninja -C $W/build src/freedreno/vulkan/libvulkan_freedreno.so
# then zip libvulkan_freedreno.so with a meta.json (schemaVersion 1, libraryName, minApi 29)
```

Checks on the result: `readelf -V` shows `LIBC`, `LIBC_N` and `LIBC_P` only, and `strings` finds the `FD616`/`FD618` entries.

## Appendix B: how the other numbers were got

- **Device counts:** gpuinfo.org's `api/internal/devices.php`, the endpoint behind the coverage pages linked above. The filter was `extension=VK_EXT_descriptor_indexing`, with `option=not` and without it, on platform android. That is 1,180 and 1,994 device names, grouped by the GPU string with Mali names matched as whole words.
- **Verdicts:** `python tools/check_device_report.py --summary <id>=<label>` on the report IDs named in the text.
- **Package checks:** each zip unpacked the way `DriverStore.import` does it. Then `readelf -d` for NEEDED, `readelf -V` for the libc versions, and `strings` for the device table.
- **The crash:** `readelf -S`/`--dyn-syms` on `libmw3.so` from the 0.1.12 release apk, against the registers and mappings in the player's log.
