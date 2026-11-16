# MW3 Android port: architecture decision

Date: 2026-11-16. Scope: native ARM64 Android build of Call of Duty: Modern
Warfare 3 (2011), first target Spec Ops Survival, test device AYN Thor.

## Verified inputs

### Disc image (read-only inspection, nothing written to it)

| Property | Value |
|---|---|
| Format | Xbox 360 XGD3, XDVDFS game partition at `0x2080000`, 8,738,846,720 bytes |
| Contents | 218 files, 8.11 GB: 100 `.ff`, 77 `.bik`, 5 `.pak`, 23 `.xex` (21 are `$SystemUpdate`) |
| Title ID / media ID | `415608CB` / `30775D59`, disc 1/1, version 0.0.0.2 (base, no title update) |
| `default.xex` | `iw5sp_ship.exe`, PowerPC big-endian, image base `0x82000000`, entry `0x82459E58`, image size `0x2330000`, linked 2011-09-19, retail AES-encrypted, basic compression |
| `default_mp.xex` | `iw5mp_ship.exe`, entry `0x824DBAE0`, image size `0x1FA0000` |
| Static libraries | D3D9 (LTCG), XAudio2, X3DAudio, XNet, XOnline are linked into the game; only `xam.xex` and `xboxkrnl.exe` are imported |
| Survival data | `common_survival.ff`, `so_survival_mp_*.ff` (16 maps) next to the `mp_*.ff` map zones and `common_specialops.ff` |

`default.xex` SHA-256 on this disc is
`e94de720373d668a2bd592dfc74339bfbbc98cca993176fcd055b1449633bcd1`. The upstream
MW3 bring-up pins `d90bc8d9...` with the same title ID and entry point. Whether
the decrypted images are identical is **not yet verified**; it is the first
check of the recompilation step.

That Survival runs from the single-player executable is inferred from the file
layout and from the PC release; it is not yet demonstrated by execution.

### Device

AYN Thor, Qualcomm QCS8550 (Snapdragon 8 Gen 2, Adreno 740), Android 13
(API 33), 4 KiB pages, 16 GB RAM, Vulkan 1.3 stock driver, 1080x1920 panel,
CPU 3x2.0 GHz + 4x2.8 GHz + 1x3.19 GHz.

## Options considered

| Approach | Evidence | Verdict |
|---|---|---|
| Xbox 360 static recompilation (XenonRecomp) + mw2-recompiled runtime | MW2 on the same engine family runs campaign and multiplayer on desktop; an Android fork reached in-game on Snapdragon handhelds (alpha); an MW3 fork already links and runs the MW3 startup to the first-run setup screens | **Chosen** |
| ReXGlue SDK | Full Xenia-derived runtime, but no Android target upstream, emulated EDRAM path reported slow on phones, no MW3 work | Reference only |
| PC x86 binary, decompile-and-rebuild (T6Requiem style) | Closed, unlicensed, depends on a multi-year decompilation that does not exist for IW5 | Rejected |
| PC binary under Wine/Box64 | Excluded by the project requirements | Rejected |
| MW32011NCP | Windows d3d9 proxy mod for the PC game; no translation technology | Not applicable |

No existing native Android or recompilation release of MW3 was found.

## Chosen architecture

```
default.xex (user's disc) -> XenonAnalyse/XenonRecomp (patched) -> C++ (never committed)
                                                                    |
                                    Android NDK clang, arm64-v8a -> libmw3.so
                                                                    |
 runtime/ (shared platform layer)                android/ (Kotlin launcher, :game process)
  kernel + XAM imports, guest memory (4 GiB)      ISO discovery and local asset install
  PM4 command processor -> Vulkan renderer        ANativeWindow surface, AAudio sink
  Xenos microcode -> SPIR-V, pipeline cache       gamepad -> XInput state, touch overlay
  guest XAudio2 + XMA decode (FFmpeg subset)      optional Turnip via libadrenotools
```

- **Base tree:** `the-drk/mw3recomp` (GPL-3.0, fork of `PaulCombal/mw2-recompiled`),
  which carries the MW3 config, hooks and kernel fixes.
- **Android layer:** ported from the `mw2-android` fork of the same runtime
  (GPL-3.0): Gradle app, `runtime/android/`, AAudio, input, render-pass fallback,
  BC decode, pipeline cache.
- **Title-specific code** stays in `config/MW3*.toml`, the MW3 block of
  `runtime/title.h` and MW3-only hooks; everything else is the shared runtime.
- **Licence:** GPL-3.0-only, inherited. Game files, the ISO, generated PPC C++
  and any binary built from it stay out of the repository.

## Known risks

1. The MW3 desktop bring-up has not reached the main menu; setup-screen
   progression is unresolved upstream.
2. ARM64 correctness of the recompiled code: memory barriers (`lwsync`/`sync`
   are no-ops in XenonRecomp), FPSCR rounding-mode mapping, setjmp/longjmp.
3. The MW3 file on this disc differs by hash from the upstream-pinned XEX.
4. Renderer performance on Adreno is unmeasured for MW3.
5. Spec Ops Survival was outside the upstream scope and is untested.
