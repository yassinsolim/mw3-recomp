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
| Jump tables | 181 (178 from XenonAnalyse, 3 from `tools/intext_switch_tables.py`) | 179 (177 and 2) |
| Functions | 18,160 | 18,289 |
| Source files written by the recompiler | 76 | 77 |
| Switch-case errors, first run | 444 at 35 sites | 503 at 39 sites |
| Leaf-function extents added | 34 | 36 |
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
- **Jump tables in the code that XenonAnalyse does not recognise.** Five
  dispatches (three in the campaign, two in the multiplayer) are
  `rlwinm r0,rN,2,0,29; lwzx r0,r12,r0; mtctr r0; bctr` with the table right
  after the `bctr` and its address formed elsewhere. Without an entry the
  recompiler took each `bctr` for a call through a pointer, and the
  multiplayer ended as a map started with "indirect call to 822F9B18 has no
  recompiled function". `tools/intext_switch_tables.py` appends an entry for
  each to the title's switch tables after XenonAnalyse has written them
  (setup runs it for this game only: the other games' tables are part of what
  their game code is checked against). Three more leaf extents followed.
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
(and `COD4_TITLE_MP`).

**Why the build also defines `MW3_TITLE`** (and CMake sets `MW2_IS_MW3`
beside `MW2_IS_COD4`). It is more than a convenience. In the runtime's sources
`MW3_TITLE` is what tells a title that is not Modern Warfare 2 from one that
is: with it the MW2-only hooks (`Com_Printf`, the memory-card prompt, the
predicate wait) and the MW2 phone build's arena workarounds are compiled out,
the heap starts above a larger image, the executable's module handle is
installed for the C runtime's start-up, and CMake builds the disc's one
executable with no title update and no profile. Call of Duty 4 needs every one
of those. Giving that meaning a name of its own would touch each of those
places in code that Modern Warfare 2 and 3 are built from, to no effect on
either, so it is left as it is and said here and at the top of
`runtime/title.h`. What `MW3_TITLE` means that is Modern Warfare 3's alone is
overridden for this title where it matters (the title id and name the
installer checks, the network tag, the script compiler, which is not built)
and otherwise harmless: its entries are compiled out with their hooks. One
thing carried over on purpose and not measured: on Android the sun's shadows
start off above a 60 frames a second limit, as for Modern Warfare 3.

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

### Windows (`setup\setup.cmd -Game cod4 -Build both`, llvm-mingw, RTX 5080)

The setup's Windows path was run end to end: it copies the whole disc (6.5 GB
with `highmip\`) into `out\pc-cod4\game`, builds `cod4.exe` and `cod4mp.exe`
and writes `Campaign.cmd`, `Multiplayer.cmd` and `.env`. The runs below are of
those executables, started as the two files start them.

| | State | Evidence |
|---|---|---|
| Campaign starts; legal, Infinity Ward, Activision and introduction movies play | **verified** | `pc-first-boot-sheet.png` |
| Title screen and the main menu | **verified** | `pc-title-press-start.png`, `pc-main-menu.png` |
| New Game: the briefing movie, F.N.G. loads and draws at 60 frames a second | **verified** | `pc-killhouse-ingame.png`, `pc-first-boot.log` |
| The pad walks, turns and fires in the level | **verified** | `pc-killhouse-movement-sheet.png` |
| The level's checkpoint is saved, and Resume Game loads it in a later run | **verified** | `pc-resume-game-sheet.png` |
| The same with no `highmip\` folder: plays, with the lower mips | **verified** | 256 `.hi` files asked for and not found |
| Multiplayer reaches its main menu | **verified** | `pc-mp-main-menu.png` |
| Multiplayer, a System Link match hosted on this machine: Shipment loads, team and class are chosen, the player spawns, walks, turns and fires, 60 frames a second | **verified** (`MW2_CONSOLE="30:set onlinegame 0;30.5:set systemlink 1;33:map mp_shipment"`) | `pc-mp-shipment-systemlink-sheet.png`, `pc-mp-shipment-systemlink.log` |
| Multiplayer with six bots (`MW2_BOTS=6`), Crash and Backlot, 400 s each: they join, spawn, fight and score; 59.4 and 59.6 frames a second in play | **verified** | `..\cod4bots\crash_scoreboard.png`, `backlot_scoreboard.png`, `crash_sheet.png`, `backlot_sheet.png` |
| A second copy joins a host full of bots (23) over `MW2_NETWORK=lan` by the host's `connect` line: a bot leaves, the player spawns | **verified** | `..\cod4bots\leaveguest_sheet.png` |
| A second player joining that match; Xbox LIVE; split screen | **open**: not tried | |

No unimplemented import was reached, no unknown GPU opcode, no guest fault in
the runs above. One warning in the campaign's level: a `WAIT_REG_MEM` given up
on after 500 ms once, during the load.

### Android (`setup\setup.cmd -Game cod4 -Build android -Dev -Suffix .dev`)

The `cod4` flavour, `com.cod4.recomp.dev` with `com.cod4.recomp.dev.game`
(30.3 MB of game code, a 10.6 MB app), the bundled Turnip driver, touch
controls, the app's defaults. The launcher finds
`/sdcard/Download/Call of Duty 4 - Modern Warfare (USA, Europe).iso` (or
`cod4.iso`) by itself.

| | AYN Thor (Adreno 740) | Retroid Pocket Flip 2 (Adreno 650) |
|---|---|---|
| INSTALL & PLAY from the disc image: the root's files, the two `.bikbe` and all 2,480 of `highmip/` (6.4 GB, 116 entries), then the game starts | **verified** (`thor-launcher-disc-detected.png`) | **verified** (`flip2-installer-highmip.png`) |
| Logos and introduction movie, title screen, main menu | **verified**, 119 FPS (`thor-main-menu-after-install.png`) | **verified**, 60 FPS (`flip2-after-install-main-menu.png`) |
| New Game: F.N.G. loads and draws; the left stick walks and the right turns | **verified** from that install, 119 FPS on the overlay (`thor-killhouse-ingame.png`, `thor-install-and-play.log`) | **verified** with the same build on files pushed by adb, which are the installer's byte for byte in count and size (`flip2-killhouse-moved.png`); not repeated after the install, the Flip 2 being shut and dozing between taps |
| Multiplayer: a System Link match hosted on the device (Crash, from the console; Backlot, from the menus) | **verified**, 119.1 FPS | **verified**, 60.0 FPS |
| Multiplayer: the Flip 2 joins the Thor's System Link match (Backlot) through a relay on the local network; both spawn, on opposite teams | **verified** (`perf-cod4\caps\sl_scoreboard_thor.jpg`, `sl_play_sheet.jpg`) | **verified** |
| Multiplayer: six bots set from the launcher's Bots entry, Crash from System Link, a ten-minute match | not run | **verified**, 60 FPS (`..\cod4bots\flip\bots_in_view.png`, `scoreboard.png`, `launcher_bots_6.png`, `game.log`) |

No guest fault, no `Com_Error`, no unimplemented import in either log.

### Android: frame rates, and what had to be fixed for them (23 November 2026)

`.dev` builds with `MW2_REPORT=1`, the app's defaults, 150 to 190 s of play
walked by one `MW2_INPUT_SCRIPT` (the same presses every run), levels started
from the console (`MW2_CONSOLE="22:devmap <level>"`). Logs and screenshots in
`C:\Users\solim\mw3-android-work\perf-cod4\`. "On time" is the share of
frames within 1.05 display periods of the one before; stutters are frames
half a blank late or more.

| | AYN Thor, 120 Hz: FPS / on time / GPU ms / consumer % of a core / stutters | Flip 2, 60 Hz |
|---|---|---|
| F.N.G. | 119.0 / 91.7% / 2.1 / 9 / 4 | 60.0 / 96.0% / 2.8 / 9 / 2 |
| Crew Expendable, first visit | 119.0 / 92.2% / 3.6 / 22 / 6 (72 pipelines compiled, none on the renderer's thread) | 59.9 / 95.3% / 8.6 / 19 / 8 |
| Crew Expendable, second visit | not run again | 59.9 / 95.5% / 8.8 / 22 / 10 (0 compiled) |
| Charlie Don't Surf, first visit | 117.7 / 87.2% / 3.8 / 32 / 48 | 59.1 / 93.8% / 2.2 / 27 / 67 |
| Charlie Don't Surf, second visit | 117.4 / 86.5% / 3.8 / 34 / 36 (0 to 3 compiled) | 59.5 / 93.1% / 3.3 / 28 / 32 |
| Crash, System Link host, alone, first visit | 119.1 / 91.9% / 2.8 / 19 / 0 | 60.0 / 96.2% / 7.2 / 19 / 7 |

Charlie Don't Surf's stutters are nearly all in the half minute the
helicopters land: the ring consumer needs 8 to 12 ms a frame there on the
Thor (5% of the run's frames) and up to 30 ms on the Flip 2. The script fires
now and then, and a shot friend ends the mission: the one pause of 1 to 3 s
in some of these runs is the checkpoint loading again, not the renderer.

What those frames are made of (25 November 2026, `MW2_REPORT_SECONDS`, which
now says a frame's draws and copies, and `simpleperf` on the ring consumer,
the Flip 2): 8,000 to 11,300 draws a frame in 90 to 120 segments, 12 to 16 MB
of vertex data, 10 to 13 MB of indices and 5 to 6 MB of constants copied, at
1.4 to 1.5 us a draw, 14 to 16 ms a frame. Of the thread's time 12% is the
vertex copies (`CopyVertices`: skinned models, whose pages the title writes
every frame, so no copy is kept; half of those bytes are a window the depth
prepass already copied in the frame), 11% the indices (`WriteIndices`, each
draw's with its own table entry in them), 8% the model lighting volume read
again at a segment's end, 8% the constants, 9% parsing the stream, and the
rest the per-draw choosing of targets, textures and pipelines. Resolves and
descriptor writes are not in it. The frame rate there is 59 to 60 with 43 to
50 frames a second on time; a run's lower average is the checkpoint loading
again.

Tried and not kept: a second thread making the vertex and index copies, the
consumer waiting for it before each report to the title (14 ms a frame to 18:
the consumer waits on a thread whose core runs slow and cold); the title's
workers let onto the efficiency cores (`T_FRAME_PROCESSORS` as Advanced
Warfare has it: 58.6 frames a second there for 59.3). Left open: a copy of a
window made earlier in the frame cannot be shown to be still what guest
memory holds without watching its pages, which costs more than the copy.

What was wrong, and is fixed:

- **"Out of memory" starting the larger levels.** The title's physical
  memory pool asks the kernel how a page is protected before it hands it out
  and starts a new 64 KB page where the answer is not what it wants.
  `MmQueryAddressProtect` said "read-write" whatever had been set, so every
  write-combined allocation took a page of its own, and Charlie Don't Surf
  came 3 MB short of the 431 MB pool (the title's own account, read out of
  its pool at the failure) and restarted the title. The kernel now keeps
  what `MmSetAddressProtect` set (`runtime/kernel/memory.cpp`, this title
  only). Crew Expendable and F.N.G. fitted before.
- **The multiplayer did not start on Android**: the app handed the
  multiplayer's library the campaign's executable (`runtime/android/jni.cpp`).
- **A System Link search found nothing.** The host answers from its game
  socket (port 1000, the console's voice and data protocol, two bytes of
  length in front) to the searcher's plain UDP socket (port 1001), which is
  handed the game data alone now (`runtime/kernel/net.cpp`, this title only).
- **A core spent waiting.** The title's back end waits awake for the sun's
  occlusion count (`sub_82108830`), which the ring consumer handed over only
  after its wait for the display: 79% of a core on the Thor in Charlie Don't
  Surf. The counts are handed over during that wait now
  (`renderer_resolve.cpp`, this title only): 39%, frame rate and frames on
  time as before (117.5 / 86.5% both ways, two runs each).
- **`MW2_INPUT_SCRIPT` on Android** pressed nothing where the on-screen pad
  answers for the first controller; it presses on top of the pad now.
- **`MW2_ENGINE_LOG=1`** shows this title's own prints (`T_Com_Printf`).

What was tried and left as it was (second visits, Charlie Don't Surf on the
Thor and Crew Expendable on the Flip 2 unless said):

| Lever | Result | Kept |
|---|---|---|
| Display-paced cap, `com_maxfps 0`, `r_vsync 0` | already applied through the console: 119 on the Thor | yes |
| Sun shadows above 60 (`MW2_SHADOWS=1`), Thor | 114.0 to 115.4 FPS, 316 to 454 stutters, GPU 4.9 ms, against 117.4, 36 and 3.8 | off, as carried over |
| Sun shadows on the Flip 2 (Charlie Don't Surf) | 55.5 FPS, 82.5% on time, against 59.5 and 93.1% | off |
| The console's 1024x600 scene on the Flip 2 (`MW2_SCENE`) | 59.8 FPS, 94.8% on time, 29 stutters, GPU 10.6 ms, against 59.9, 95.5%, 10 and 8.8 | 832x480 |
| Texel forms off (`MW2_VERTEX_TEXELS=0`, `MW2_PIXEL_TEXELS=0`) | Thor GPU 4.5 ms for 3.8; Flip 2 59.2 FPS, 90.2% on time, GPU 12.7 ms | on |
| Pipeline workers and saving | a second visit compiles 0 to 3 pipelines, none on the renderer's thread | as they are |

The opening movies: the on-screen Start skips each one (verified on both;
the main menu is reached in about 20 s with four presses), and New Game is in
F.N.G. eight seconds later. The earlier note that it did not was a press
made too late to tell.

The release-signed app (`com.cod4.recomp`, built from `82462c5`, with the
game package under that name) was installed on both devices with the game
files copied over. On the Thor it starts, New Game loads F.N.G. and plays at
119 FPS on the overlay, with 6 stutter lines in 100 s of a first visit, the
longest frame 32 ms. A release build takes no switches (no `options.txt`, and
`.env` is not read under Android), so no report, no console and no script:
the heavier levels, the multiplayer and the Flip 2 were **not** run on it.

The first published build, v0.1.0 (`yassinsolim/cod4-recomp`, a draft on 23
November, rebuilt that day from `6137d2a`: `cod4-recomp-v0.1.0.apk`, version
code 100, the release certificate, no recompiled code in it), states the same
game code as the package built here (`cod4 94ec5f58a992`, `cod4mp
a4c71519c558`), and so does a package setup built from a fresh clone of the
public repository (**verified**: `setup\setup.cmd -Game cod4 -Iso ... -Install
-Yes` from a CRLF checkout, the Thor with no Call of Duty 4 package on it).
A draft is invisible to setup, which then builds the app itself (version code
104, this PC's key) and says so; the published path is Modern Warfare 2's,
and the draft's asset names are the ones it looks for.

As a new player has it, on the release build, by taps (**verified**, 23
November; screenshots `perf-cod4/caps/t_np_*`, `f_rel_*`, `m2_*`, `u_flip*`):

| | AYN Thor | Flip 2 |
|---|---|---|
| INSTALL & PLAY from `/sdcard/Download` | 6.4 GB in about 70 s, then the game | (installed before) |
| F.N.G. | 118 to 119 FPS | 60 |
| heavy level, from a save resumed in the menu | Charlie Don't Surf: 113 to 117 on the flight in, then 119 | Crew Expendable, first visit: 57 to 61 |
| System Link, hosted alone | Backlot 119 | Crash 60 to 61 |
| the two in one match through `mw3.yassin.app:3074` (the default) | host, 119 | joined from the list, 60 |

That match is a whole ten-minute Team Deathmatch to its time limit, each on
the other's scoreboard. Before the enumerator fix (CHANGELOG) it ended twice
at 319 s of the host's uptime. The multiplayer's pause menu has no rank or
unlock entries. The app's update was seen whole on the Flip 2: a debug build
under the published name (code 99, signed again with the release key, asking
`tools/serve_test_release.py --game cod4`) offered v0.1.0 with its notes,
downloaded the draft's own APK and installed it over itself; the 2,595 game
files and every save's checksum were the same afterwards. The notes show
their Markdown asterisks as written. **Open**: the launcher's fix for the
second start after INSTALL & PLAY was not seen on a device.

Going from the app built here (code 104) to the published one (100) is a
downgrade to Android, so the app's folder was moved aside for the uninstall,
as setup does. On the Flip 2 (Android 13) the place setup had for it,
`/sdcard/mw3-recomp-keep-<package>`, was not a rename but a copy of all 6.4 GB
at about 8 MB a second. Setup now moves it beside the app's own folder,
`/sdcard/Android/data/mw3-recomp-keep-<package>`, which is a rename
(**verified** with setup's own `Install-Package` and an app signed with
another key over `com.cod4.recomp.dev`: 1.4 s on the Flip 2, 0.8 s on the
Thor, the 2,607 files and every save's checksum the same afterwards, and the
new app, under another user id, reads the game files and writes its logs).

### Modern Warfare 2 and 3 are as they were

The shared files this title changed (`CMakeLists.txt`, `cmake/`,
`runtime/title.h`, the installer, `launcher/disc.*`, `tools/fixbounds.py`,
`tools/title.py`, `setup/setup.ps1`, the app) were checked on this branch
against `android`:

- **The game library statements are byte-identical** for `mw3`, `mw3mp`, `mw2`
  and `mw2mp` (`49cb73ddd14a`, `ea9e4d15ac5e`, `d5de1f8e8711`, `94e6825e2bf7`):
  `game_abi.cpp` of a game configure of each compared with `android`'s, and
  the four `game_abi_expected.h` of the apps built here with `android`'s. No
  player has to build game code again.
- **`tools/fixbounds.py` prints the same** as `android`'s for all four
  executables, run on a recompile of each without its extents. The first
  version of the change did not: it took the exception handlers' labels inside
  three of Modern Warfare 3's functions for starts, which is why an address
  inside a function `.pdata` describes is now left alone.
- **Both build and start on Windows**: Modern Warfare 3's campaign and Modern
  Warfare 2's (the disc's version) built from this branch each ran 45 seconds
  at 60 frames a second to their first screen (`regress-mw3-mw2-pc.png`,
  `regress-mw3-pc.log`, `regress-mw2-pc.log`).
- **Both apps build**: `assembleMw3Tu0Debug` and `assembleMw2Tu0Debug`.

## Open

- Split screen; the local-network backend.
- Xbox LIVE through a relay: Find Match (Team Deathmatch) plays, between the
  two devices on the draft `3c10903` and between two Windows copies
  ([multiplayer.md](multiplayer.md#verified-through-the-public-relay)). It
  needed playlists of this title's own maps (`relay/playlists_cod4.info`),
  an answer to `XUserReadStats` the title takes (the host of a team lobby
  reads everyone's skill, and without one view in the answer stays at "Match
  beginning in: 0"; the errors go through a print the log does not show),
  and a tie broken between two players who search in the same second, which
  the draft does not have yet. Free-for-all, a private match, invitations and
  a party of two searching are **not** run.
- The public relay (`mw3.yassin.app:3074`) from the Flip 2: on 23 November
  one run's Hellos went unanswered for the five seconds a copy asks
  (`logs_mp_crash_cold_flip.log`). The likely cause, found later the same
  day: every first copy came to the relay from port 3075, and a second
  machine behind the same router got no answer from it (reproduced from the
  PC, port by port, while the Thor was in a match). A copy now uses a port
  the system picks ([multiplayer.md](multiplayer.md#lan-and-server)); the
  draft `3c10903` has that. A copy still asks once, at start: after such a
  start the multiplayer is offline until it is started again.
- Bots: the multiplayer has them, as the two later games do
  ([multiplayer.md](multiplayer.md#call-of-duty-4s-bots)). Not run: the AYN
  Thor, and a device joining an Android host that is full of bots.
- Unlocks: none, and none needed where it was looked. A System Link match
  offers ten ready-made classes (Grenadier to Sharpshooter) and no Create a
  Class, so nothing is locked there. What a rank locks is Create a Class on
  Xbox LIVE: the menus ask the stats through two expression operators,
  `stat` (`sub_821F4968`) and `statRangeAnyBitsSet` (`sub_821F4830`), both
  over `sub_821A1CC8(controller, index)`; a weapon's stat is 3000 and up, its
  low sixteen bits the weapon, its attachments and its camouflages (derived
  from the stock scripts' unlock functions, not run). Answering those two
  with everything set would be the client-side switch; not done, there being
  no ranked menu reached to check it against.
- The published app played by hand: the levels, the multiplayer, the two
  devices in one match (above), and a way to give a release build
  `MW2_REPORT`.
- Charlie Don't Surf's landing, bound by the ring consumer on both devices.
- The Live lobby's Invite and Join entries of the app's pause menu, which
  this title has as Modern Warfare 2's multiplayer does; not tried. (The
  rank, unlock and prestige entries, which write Modern Warfare 2's stats
  file, are no longer offered to it.)
- The frame's shape (`MW2_RENDER`), the shader hand-over at load, the field of
  view and the other optional hooks listed under the title blocks.
- Whether a title update exists and matters.
