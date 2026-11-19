# Changelog

All notable changes to the Modern Warfare 2 Android native recompilation project will be documented in this file.

## [Unreleased]

### Added
- **GPUs without subgroup ballot in pixel shaders** (the Retroid Pocket
  Flip 2's Adreno 650 under Turnip) draw the game instead of running it with
  a black screen: there a pixel shader reads its own draw record and indexes
  its textures non-uniformly, the form before the waterfall. Devices with
  ballot keep the waterfall; the log names the form at start
  ([docs/shaders.md](docs/shaders.md)).
- **Adreno 6xx (Retroid Pocket Flip 2): a level no longer resets the
  device, and Modern Warfare 3 holds 30 frames a second there.** Turnip is
  started without image compression (`TU_DEBUG=noubwc`), which hard-reset
  the device as a level's world was first drawn; the FPS cap starts at 30
  and the sun's shadows off (`MW2_SHADOWS`), the two things that turn 25
  uneven frames a second into 30 even ones
  ([docs/android.md](docs/android.md#adreno-6xx-snapdragon-865-class)).
- **`MW2_FPS_LIMIT=30` is the console's pacing at half rate**: every frame
  on screen for two blanks of a 60 Hz display, instead of the title's
  limiter, whose 33 ms frames showed for one blank or three every second or
  so ([docs/switches.md](docs/switches.md)).
- **MW3 multiplayer: bots** (`MW2_BOTS=<n>`, `MW2_BOT_SKILL`, `MW2_BOT_TEAMS`,
  `runtime/bots.cpp`; the app's "Bots in my matches"): the host's server adds
  computer players through the test-client path the executable kept, they
  choose a team and a class through the stock menus, shoot at the enemies
  they see and walk towards the nearest one. Host only; a bot leaves when a
  player needs its slot. `bots <n>`, `bots +`, `bots -` on the host's console
  ([docs/switches.md](docs/switches.md)).
- **MW3 multiplayer: voice chat** (`MW2_VOICE=1`, `runtime/kernel/voice.cpp`;
  the app's "Voice chat", off by default): the headset the title's voice
  library asks XAM for, made of the microphone (SDL, or AAudio on Android)
  and the game's sound output. Push-to-talk by default (T, or the on-screen
  Talk button); the Android app asks for the microphone only when voice chat
  is turned on. Speech passes through the relay unencrypted. Compiled, not
  yet run ([docs/multiplayer.md](docs/multiplayer.md#voice-chat)).
- **One-command Windows setup** (`setup/setup.cmd`, `setup/setup.ps1`): fetches
  pinned, hash-checked tools into `.toolchain/`, asks for the player's own disc
  image, checks its executables (`tools/check_mw3_xex.py`), recompiles them and
  builds the Android apps and/or the Windows game, then optionally installs
  them over adb; `-Check` reports prerequisites without changing anything.
  The README is rewritten for players (status, quick start, online play,
  credits, disclaimer); `THIRD_PARTY.md` lists every component's licence.
- **Skin tools** (`tools/iwi_match.py`, `tools/camo_gen.py`): match texture
  dumps to the user's own PC `.iwi` files and write the user's modded images
  under the dumps' keys; draw original procedural camos (carbon, voronoi,
  marble, digital, iridescent, plasma, animated variants)
  ([docs/textures.md](docs/textures.md#making-skins)).
- **MW3 multiplayer: unlock everything** (`MW2_UNLOCK_ALL=1`,
  `runtime/mw3_unlock_all.cpp`; the app's "Unlock all camos and items", on by
  default): the title's unlock checks answer yes, in the menus and where a
  match checks a spawning player's class, so a chosen camouflage is on the
  weapon in play; the stats are not changed
  ([docs/MW3-MP-BRINGUP.md](docs/MW3-MP-BRINGUP.md#everything-unlocked)).
- **Modern Warfare 3's multiplayer executable builds as a title of its own**
  (`MW2_TITLE=mw3mp`, `config/MW3MP.toml`, `runtime/title.h`, the `mw3mp`
  Android flavour, `com.mw3.recomp.mp`): recompiled, linked and packaged, not
  yet run, and without a network
  ([docs/MW3-MP-BRINGUP.md](docs/MW3-MP-BRINGUP.md)). The recompiler patch
  gained the nine VMX halfword instructions that executable uses.
- **MW3 multiplayer network hardening** (`runtime/title.h`, the fix files of
  [docs/security.md](docs/security.md#modern-warfare-3-multiplayer)): MW2's
  multiplayer fixes located in `iw5mp_ship.exe` and wired, three shown absent
  in IW5, and two IW5-only fixes (`runtime/partystate_fragment_index.cpp`, the
  snapshot glass state in `runtime/glass_piece_count.cpp`). Compiled, not yet
  run; the title's network stays off.
- **Texture replacement (skins)** (`runtime/gpu/texture_replace.cpp`,
  `runtime/png.cpp`, `runtime/gpu/vulkan/texture_cache.cpp`): a PNG or PNM
  named by a texture's content key, in `replace/` in the game folder or
  `MW2_TEXTURE_REPLACE=<dir>`, is uploaded in that texture's place.
  `MW2_DUMP_TEXTURES=<dir>` now works in every build and names its files by
  that key, as PNG ([docs/textures.md](docs/textures.md#texture-replacement-skins)).

### Fixed
- **The Windows build holds the mouse** (`runtime/gpu/vulkan/presenter.cpp`,
  `runtime/kernel/input.cpp`): the cursor used to walk out of the window as
  the player looked round, and stop turning the view at the screen's edge.
  The window now takes the mouse in SDL's relative mode while it has the
  keyboard; `Alt+Tab` or `F2` lets it go. Mouse look is a turn of so many
  degrees a count (`MW2_MOUSE_SENSITIVITY`) whatever the frame rate, and a
  slow hand is no longer lost in the stick's dead zone
  ([docs/runtime.md](docs/runtime.md#input)).

### Changed
- **Consecutive draws that change no state are one hardware draw**
  (`runtime/gpu/vulkan/renderer_draw.cpp`, `renderer_arena.cpp`,
  `shader_translator.cpp`): the GPU's cost on Adreno is per hardware draw, and
  an entry of a `vkCmdDrawMultiIndexedEXT` was one. A run of indexed draws is
  now one `vkCmdDrawIndexed` over 32-bit indices that name each draw's entry in
  a draw table, through which a vertex finds its draw's record
  ([docs/rendering.md](docs/rendering.md#the-recorder-thread)). The renderer
  now needs non-uniform indexing of sampled images as well as subgroup ballot
  in pixel shaders. `MW2_NO_MERGE=1` makes every draw a hardware draw of its
  own. Cached shaders of earlier builds are not reused.
- **A pixel shader runs once for each draw its subgroup holds**
  (`shader_translator.cpp`, [docs/shaders.md](docs/shaders.md#the-module-interface)):
  merged draws had made the record, and with it every constant's address and
  texture index, a value per pixel, which nearly doubled the pixel work of a
  frame on Adreno. The program is now the body of a loop that takes one
  record a pass, so they are one value for the subgroup again.

## [v0.0.2-alpha] - 2026-10-18

### Highlights
- **Release tags with a suffix are now read.** `v0.0.2-alpha` was a tag the
  launcher's update check could not parse at all, so it answered "its
  description is not one" and no player was ever offered it — as every
  `v0.1.0-alpha.N` release before this one went unoffered.
- The apk says which release it is, instead of "1.0".
- Faster software decoding of block-compressed textures on Mali and Adreno
  (BC2 10.1 → 4.7 ms per 2048x2048 decode).

### Added
- **`launcher/version.h`**: release tags compared as semantic versioning orders
  them — `0.0.2-alpha < 0.0.2-beta < 0.0.2 < 0.0.3-alpha`, with `alpha.10` after
  `alpha.9` and not before it, and a tag that is not a version refused rather
  than ordered somewhere. `update::Look` uses it (`launcher/update.cpp`).
- **`tests/version_test.cpp`** (`ctest` name `version`): the order above, the
  project's own published tags, and the shapes that are not versions.
- **`tools/bc_bench.cpp`**, built as `bc-bench`: how long one 2048x2048
  software BC decode takes per format. The figures quoted in
  `runtime/gpu/vulkan/bc_decoder.cpp` come from it.
- **`MW2_VERSION_NAME`** (and `MW2_VERSION_CODE`) in `android/app/build.gradle.kts`:
  the release the apk was built from, taken from the tag by `build-apk.yml`.

### Changed
- **BC decoder, row at a time** (`runtime/gpu/vulkan/bc_decoder.cpp`): BC2, BC3
  and BC5 walked all sixteen pixels of a block with a computed x, y and shift;
  they now take a row from a word already loaded, with the block's masked
  colours and lifted alphas made once. On a 2048x2048 decode (best of 12,
  `bc-bench`): BC2 10.1 → 4.7 ms, BC3 7.1 → 6.4 ms, BC5 6.7 → 6.4 ms, BC1
  unchanged at 3.8 ms. The output is byte for byte what it was —
  `tests/bc_decoder_test.cpp` compares it against a decoder written from the
  formats themselves.
- **`versionCode` 15 → 16**, so Android installs this over v0.0.2.
- **`.github/workflows/release.yml`**: a tag that is not a version now fails the
  run before anything is published — a release tag the launcher cannot read is
  a release nobody is offered — and a tag with a suffix after its version is
  drafted as a pre-release, which is what keeps GitHub's "latest release" (the
  one the launcher asks for) on a version everyone can install.
- **`docs/building.md`**: the list of harnesses and `ctest`'s checks, which said
  three targets where there were four and now are five.

### Fixed
- **A build with no release tag no longer compares itself against the newest
  release.** The empty version parsed as no numbers, which every version sorts
  above, so an unreleased build was offered an update it should not have been
  (`update.h` says such a build does not update; now it does not).

### Notes
- GitHub's "latest release" — the endpoint the launcher reads — is never a
  pre-release, so publishing `v0.0.2-alpha` as a pre-release reaches players
  through the releases page and the apk, not through the in-launcher updater.
  That is the intended behaviour for an alpha; a pre-release build being
  *offered* pre-releases would need the list endpoint instead.

---

## [v0.0.2] - 2026-10-13

### Highlights
- Comprehensive fixes for Qualcomm Snapdragon (Adreno) chips.
- Compatibility floor lowered to **Vulkan 1.0**.
- ARM64 CPU thread affinity for Prime (Cortex-X) cores.
- TBDR mobile tile rendering optimizations.
- Virtual Filesystem (VFS) mod directory overlay (`mods/`).
- Ingestion security hardening for disc extraction and archives.

### Added
- **ARM64 CPU Affinity**: Pinned main game loop to the Prime Core (Cortex-X), command recorder to Performance Cores, and background worker to Efficiency Cores via Bionic `sched_setaffinity` (`runtime/platform.cpp`).
- **Mod Directory Overlay**: Added support for offline Bot Warfare GSC mods and loose assets located in `MW2_MOD_DIR` or `<GameRoot>/mods/` (`runtime/kernel/files.cpp`).
- **Pillar Architecture Documentation**: Created technical specifications for CPU affinity, Vulkan mobile pipeline, memory budgeting, and bots/netcode in `docs/pillars/`.
- **In-Game FOV Control**: Merged upstream FOV slider support and camera adjustments.

### Fixed
- **Snapdragon / Adreno BC Texture Bug**:
  - Automatically routes BC1–BC5 textures through SIMD software decoding on Qualcomm GPUs (`vendorID == 0x5143`) to avoid hardware 1-bit alpha sampling defects.
  - Representation-safe texture uploads and restaging in `runtime/gpu/vulkan/texture_cache.cpp`: decompression failures are explicitly caught and reported, preventing black terrain, flashing vegetation, and black skybox artifacts.
- **Vulkan 1.0 Support**:
  - Manifest hardware requirement lowered to `0x400000` (`android/app/src/main/AndroidManifest.xml`).
  - Dynamic capability detection and fallback to classic Vulkan 1.0 render passes and negative viewport extensions.
- **TBDR Mobile RenderPass Optimization**:
  - Configured `VK_ATTACHMENT_STORE_OP_DONT_CARE` on stencil and depth targets in `runtime/gpu/vulkan/renderer_targets.cpp`, eliminating tile memory spilling.
- **Archive Extraction Hardening**:
  - Fixed order-of-operations bug in `tools/fetch_asset.py` where temporary file was deleted before reading header bytes.
  - Implemented zip-bomb bounds (50,000 entries and 15 GiB limit).
- **XDVDFS Disc Extraction Hardening**:
  - Added sector bounds validation against total image size (`tools/xdvdfs.py`).
  - Added directory cycle detection and recursion depth limits.
  - Enforced destination path containment using `os.path.realpath` to prevent path traversal.
  - Added truncated read validation.
- **CI & Release Determinism**:
  - Made manual APK builds artifact-only by default and enforced explicit release tag validation (`.github/workflows/build-apk.yml`).

---

## [v0.0.1] - 2026-10-10

### Added
- Initial release of the native Modern Warfare 2 recompilation port for Android ARM64 (`com.mw2.recomp.sp`).
- XenonRecomp static PowerPC recompiler generating 64-bit ARM machine code (`libmw2.so`).
- Vulkan renderer backend supporting mobile TBDR architectures.
- Low-latency AAudio native sound sink at 48 kHz stereo.
- Integrated Android launcher with on-screen touch controls, gamepad support, and XDVDFS ISO installer.
- Custom GPU driver installer (`libadrenotools`) in `DriverStore.kt`.
