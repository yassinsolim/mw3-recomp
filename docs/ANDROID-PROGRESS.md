# Android port progress log

Status words: **verified** (observed on the device or by a run), **built**
(compiles/links only), **not started**.

| System | Status |
|---|---|
| Disc inspection (format, XEX identity, asset inventory) | verified |
| Thor detected over ADB, hardware inventory | verified |
| Architecture selected | done, see [ANDROID-ARCHITECTURE.md](ANDROID-ARCHITECTURE.md) |
| Recompilation of this disc's `default.xex` | not started |
| Android runtime (`runtime/android/`), Gradle app | not started |
| Guest code executing on the Thor | not started |
| Rendering, audio, input on the Thor | not started |
| Main menu, Survival map load, gameplay | not started |

## 2026-11-16

- Inspected the ISO read-only: XGD3, title `415608CB`, `iw5sp_ship.exe` /
  `iw5mp_ship.exe`, base version, 16 Survival zones present.
- Thor: QCS8550, Android 13, Vulkan 1.3, 16 GB RAM, 4 KiB pages.
- Surveyed XenonRecomp, mw2-recompiled, the mw2-android fork, ReXGlue,
  MW32011NCP and T6Requiem; no existing MW3 Android port found.
- Repository created from `the-drk/mw3recomp` history (GPL-3.0).
