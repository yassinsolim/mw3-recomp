# Call of Duty 4: Modern Warfare: bring-up

The third title on this runtime, after Modern Warfare 2 and Modern Warfare 3
([MW3-BRINGUP.md](MW3-BRINGUP.md), [MW2-ANDROID.md](MW2-ANDROID.md)): the
Xbox 360 disc of 2007, the IW3 engine, IW4's predecessor. This is the log of
what was found, what was done and what has been seen to work. A statement here
is one of three things, and says which: **verified** (run, with what was
seen), **derived** (read out of the executable, not run), or **open**.

Nothing from the disc is in the repository: `cod4/` (the executables and the
game files), `ppc_cod4/`, `ppc_cod4_mp/` and the switch tables are
git-ignored, as the other titles' are (`tools/check_no_game_files.py`).

## The disc

`Call of Duty 4 - Modern Warfare (USA, Europe).iso`, 7,835,492,352 bytes, read
with `tools/xdvdfs.py` and never written to. The XDVDFS partition starts at
`0xFD90000`; the root directory is at sector 1783933.

| | |
|---|---|
| Files | 2,596 in 6.94 GB: 115 in the root, 2,480 in `highmip/`, one in `$SystemUpdate/` |
| Fastfiles (`.ff`) | 60: `code_post_gfx`, `ui`, `common` and their `_mp` twins; 21 campaign levels (`killhouse` to `simplecredits`, 45 to 138 MB each); 16 multiplayer maps (`mp_backlot` to `mp_vacant`, 43 to 83 MB) each with a `_load.ff` |
| Image pack | one, `packfile1.pak`, 830 MB |
| High mips | `highmip/*.hi`, 2,480 files, 1.15 GB: the top mip of each streamed texture |
| Movies | 50 `.bik` (logos, `cod_intro`, a `_load` and a `_fade` for each level) and two `.bikbe` |
| Executables | `default.xex` (4,145,152 bytes) and `default_mp.xex` (4,321,280 bytes) |

Both executables (`tools/xexinfo.py`):

| | `default.xex` | `default_mp.xex` |
|---|---|---|
| Original name | `CoD3.exe` | `CoD3MP.exe` |
| SHA-256 | `f6b848ac4ecf2b12881223e4c3cd63c49ae36e1fadc6f2179428033274602739` | `9aae8db5b38f0b394f9052d1ee9c8b1f18038ff86fc5f4110f173f66043a0cc6` |
| Title id | `415607E6` | `415607E6` |
| Media id (execution info) | `15E8CF88` | `15E8CF88` |
| Version, base version | `00000001`, `00000001` | `00000001`, `00000001` |
| Time stamp | 2007-09-08 00:09:19 UTC | 2007-09-08 00:11:45 UTC |
| Image base, size | `82000000`, `1AE0000` (28.2 MB) | `82000000`, `33D0000` (54.3 MB) |
| Entry point | `820E4600` | `82103AC8` |
| `.text` | `82090000`, `2A4F1C` bytes | `820A0000`, `2C5D94` bytes |
| Compression, encryption | raw blocks, retail key | the same |
| Static libraries (XDK 2.0.6132.64) | D3DX9, XGRAPHC, XAPILIB, XBOXKRNL, XNET, XONLINE, LIBCMT, D3D9LTCG, XAUDLTCG, X3DAUDL | the same and XHV |
| Imports | `xam.xex`, `xboxkrnl.exe`: 173 functions | 198 functions |

It is the disc's own build and not a title update: version and base version
are the same `1`, the files come from the disc image, and there is no patch
descriptor in the header. Whether a title update exists for this disc, and
what it changes, is **open**; the port is of the disc.

What differs from the two later games, and matters to the runtime:

- **Bink is linked in** (the `BINK`, `BINKBSS` and `BINKDATA` sections): the
  title decodes its own movies and draws them as textures. Nothing of the
  runtime's movie code is used, and the logos, the introduction and the
  loading movies play (verified, below).
- **Direct3D 9 and the audio library are two years older** (XDK 6132 against
  MW2's 7978), but the same designs: the ring buffer, the arena with its
  progress block and the flip handshake of [d3d9-seam.md](d3d9-seam.md) are
  all there, and the audio goes through the same `XAudioRegisterRenderDriverClient`
  / `XAudioSubmitRenderDriverFrame` pair. No renderer or audio change was needed.
- **Every import but four is implemented already.** The four
  (`DbgBreakPoint`, `NtQueryDirectoryFile`, `RtlUnwind`,
  `__C_specific_handler`; the multiplayer adds `XamShowKeyboardUI` and
  `XamShowMarketplaceUI`) are logging stubs, and none was reached in any run
  below.

`tools/check_cod4_xex.py` accepts these two files by SHA-256 and nothing else,
as `tools/check_mw3_xex.py` does for Modern Warfare 3.

## Recompiling

`setup\setup.cmd -Game cod4` does all of it; by hand it is the steps of
[MW3-BRINGUP.md](MW3-BRINGUP.md) with `MW2_TITLE=cod4` (or `cod4mp`),
`config/COD4.toml` (`COD4MP.toml`) and `cod4/tu0/`.

| | campaign | multiplayer |
|---|---|---|
| Jump tables (XenonAnalyse) | 178 | 177 |
| Functions | 18,160 | 18,289 |
| Source files written by the recompiler | 76 | 77 |
| Switch-case errors, first run | 444 at 35 sites | 503 at 39 sites |
| Leaf-function extents added | 32 | 35 |
| Switch-case errors now | 0 | 0 |
| Instructions the recompiler does not know | 9 (stepped over, below) | 9 |

- **The helpers.** The eight register save/restore helpers, `setjmp` and
  `longjmp` were found by `tools/port_hooks.py --version tu0` from MW2's disc
  executable: each matches once, with MW2's size.
- **Leaf functions with jump tables** are given their extents by
  `tools/fixbounds.py`, as for the other titles. Call of Duty 4 showed a case
  the tool did not have: two such functions one after the other, the second
  reached only through a table of function pointers. With no `bl` to it the
  two came out as one function, and the first level's load ended in "indirect
  call to 8232C958 has no recompiled function". The tool now also takes as a
  function start every word of `.rdata` and `.data` that is the address of an
  instruction and not a case label; two extents of the campaign and one of the
  multiplayer came out differently for it.
- **`vandc` and `vctuxs`.** The pinned recompiler has the VMX128 forms of these
  two AltiVec instructions and not the plain ones, which neither later game
  uses. Nine instructions in three functions of each executable. Each is
  replaced by a call (`[[midasm_hook]]` in the configuration,
  `runtime/recomp/ppc_extra_instructions.cpp`) and stepped over; the
  recompiler still reports each as unrecognised, which is why setup expects 9
  and not 0. Two more `case` labels in `patches/xenonrecomp-mw2.patch` would
  be the plainer fix, but the patch's hash is part of what every title's game
  library is checked against ([game-library.md](game-library.md)), so that
  waits for a change that breaks that statement anyway.

## The title blocks

`runtime/title.h` has a block for each executable, selected by `COD4_TITLE`
(and `COD4_TITLE_MP`). The build also defines `MW3_TITLE`, which the runtime's
sources read as "not Modern Warfare 2: a hook without an entry is compiled
out"; CMake's `MW2_IS_COD4` beside `MW2_IS_MW3` is the same arrangement.

Every address was found in Call of Duty 4's own executable, by laying the
function beside the one MW2's disc block names (`ppc_tu0/`):

| Entry | MW2 disc | CoD4 campaign | CoD4 multiplayer | Found by |
|---|---|---|---|---|
| `T_D3D_Present` | `820C3390` | `820A0FF0` | `820B08D0` | the one caller of `VdSwap`, with the same chain of calls and imports around it |
| `T_D3D_ArenaWait` | `820B9800` | `8209A0E0` | `820A99C0` | (device, position, sequence); first load `[device+10896]`; the wait between the same two calls |
| `T_DEV_PresentInterval` | 13596 | 13580 | 13580 | the flip packet writer (`820A0AC0`, MW2's `820C2E60`) reads 10900, this, 11844 (MW2 11860), 10900, 10941 in MW2's order |
| `T_DEV_ProgressBlock` | 10896 | 10896 | 10896 | the arena wait's first load |
| `T_Com_Error` | `822830E8` | `821DB7F0` | `82234308` | the one function that leaves by `longjmp` past a 4096-byte format buffer; 277 and 303 callers |
| `T_Cbuf_AddText` | `8227CF18` | `821DE9D8` | `82237C50` | the "p<digit> " client prefix parse; called with `exec default.cfg` at start-up |
| `T_R_VideoSizes` | `8238E5D0` | `82135318` | `8216CCD0` | the one caller of `XGetVideoMode`; stores 1024x600 at +8 and 1280x720 at +16 as MW2's does |
| `T_DATA_TimeStampBundlePtr`, `DebugMonitorPtr`, `ModuleHandlePtr` | | `82000518`, `820005CC`, `82000550` | `82000764`, `82000820`, `82000798` | the image's import records `000100AD`, `00010059`, `00010193`, each present once |

The multiplayer's functions were found from the campaign's by their sequence
of instructions, each matched once.

- **The frame-rate dvar** is `com_maxfps`, as in the later games, with
  `r_vsync` and `sm_enable` beside it: the three strings are in the image, and
  the runtime's pacing (`gpu/d3d_hooks.cpp`) sets them through
  `T_Cbuf_AddText` unchanged.
- **Video sizes:** `T_R_VideoSizes` is hooked, so `MW2_SCENE` applies
  (derived; not measured). The frame's own shape (`T_R_SetVidConfig`,
  `MW2_RENDER`) is **open**.
- **File-system paths:** the title opens everything as `D:\<name>` and
  `D:\highmip\<name>.hi`, and its save as `save:\savegame.svg`; `update:\` is
  named in the fastfile loader and was not asked for in any run. The kernel's
  file layer serves all of these as it is.
- **The guest heap** starts at `86000000` for the multiplayer, whose image
  ends at `853D0000` (`runtime/guest_memory.h`).
- **Not located**, each compiled out: the shader hand-over at load
  (`T_D3D_InitPixelShader` is inlined in this D3D; `T_D3D_PatchVertexFetches`
  has one candidate, `820A22F8`, not enabled), so shaders are compiled at
  their first draw; the memory-card prompt, the predicate wait, the field of
  view, the mouse and the mods' entries.

## What has been seen to work

Evidence is kept outside the repository, in
`C:\Users\solim\mw3-android-work\private-captures\cod4\`.

### Windows (llvm-mingw build, RTX 5080)

| | State | Evidence |
|---|---|---|
| Campaign starts; legal, Infinity Ward, Activision and introduction movies play | **verified** | `pc-first-boot-sheet.png` |
| Title screen ("Press START") and the main menu | **verified** | `pc-title-press-start.png`, `pc-main-menu.png` |
| New Game: the loading movie, `killhouse.ff` (F.N.G.) loads, the level draws with its HUD at 60 frames a second | **verified** | `pc-killhouse-ingame.png`, `pc-first-boot.log` |
| The same with no `highmip/` folder: plays, with the lower mips | **verified** | run of `out\pc\cod4.exe`, 256 `.hi` files asked for and not found |
| Multiplayer starts and reaches its main menu (Xbox LIVE, Split Screen, System Link) | **verified** | `pc-mp-main-menu.png`, `pc-mp-first-boot.log` |
| Multiplayer, Xbox LIVE: "You must have an active network connection" (`Com_Error(1) XBOXLIVE_NETCONNECTION`), as expected with no network backend chosen | **verified** | the same log |
| A multiplayer map | **open**: not tried | |

No unimplemented import was reached, no unknown GPU opcode, no guest fault.
One warning in the campaign's level: a `WAIT_REG_MEM` given up on after 500 ms
once, during the load.

### Android (the `cod4` flavour, `com.cod4.recomp.dev` with `com.cod4.recomp.dev.game`)

Built by `setup\setup.cmd -Game cod4 -Build android -Dev -Suffix .dev`: the
recompile (the numbers above), both game libraries (30.3 MB package), the app
(9.3 MB), installed with adb. The game files were pushed into the app's game
folder with adb, not copied by the app's installer, which was **not run**.
Bundled Turnip driver, touch controls, the app's defaults.

| | Retroid Pocket Flip 2 (Adreno 650) | AYN Thor (Adreno 740) |
|---|---|---|
| Starts; logos and introduction movie | **verified**, 60 FPS (`flip2-intro-movie.png`) | **verified**, 119 FPS |
| Title screen | **verified** (`flip2-title.png`) | **verified** (`thor-title.png`) |
| Main menu | **verified** (`flip2-main-menu.png`) | **verified** (`thor-main-menu.png`), 119 FPS |
| New Game: F.N.G. loads and draws with its HUD | **verified**, 60 FPS on the overlay (`flip2-killhouse-ingame.png`) | **open**: the scripted presses of A (`input swipe`) were not taken and the run was ended there; not a fault seen, not a level seen |
| The left stick moves the player | **verified** (`flip2-killhouse-moved.png`) | **open** |
| With `highmip/` | yes, all 2,480 files | no: the push ended before that folder, so this is the installer's set of files |
| Multiplayer | **open**: not started | **open** |

Logs: `flip2-game.log`, `thor-game.log`. No guest fault, no `Com_Error`, no
unimplemented import in either; three stutter warnings on the Flip 2 as the
level's textures were first read. No frame-rate measurement was made beyond
the overlay's number.

## Open

- The app's installer copies the disc's root (`.ff`, `.pak`, `.bik`,
  `.bikbe` and the two executables) and not `highmip/`: the game plays
  without it, with lower-detail textures. `setup -Game cod4 -Build pc` copies
  the whole disc.
- A multiplayer map (split screen or system link), and the network backends
  for this title.
- The frame's shape, the shader hand-over at load, the field of view and the
  other optional hooks listed above.
- Whether a title update exists and matters.
