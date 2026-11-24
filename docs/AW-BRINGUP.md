# Call of Duty: Advanced Warfare: bring-up

The fourth title on this runtime: the Xbox 360 game of 2014, Sledgehammer's S1
engine as High Moon Studios built it for this console. This is the log of what
was found, what was done and what has been seen to work. A statement here is
one of three things, and says which: **verified** (run, with what was seen),
**derived** (read out of the discs or the executables, not run), or **open**.

Nothing from the discs is in the repository: `aw/` (the executables),
`ppc_aw/`, `ppc_aw_mp/` and the switch tables are git-ignored, as the other
titles' are, and `tools/check_no_game_files.py` refuses them.

State on 24 November 2026, in one line: both discs read, both executables
recompile clean, the install disc's packages are mounted as the game expects,
and the campaign reaches its main menu and plays its first level (Seoul) with
the right exposure on Windows at 60 frames a second and on the AYN Thor at
119; the multiplayer reaches its main menu on Windows. A first run goes
Brightness, Safe Area, main menu, each of the two asking for an adjustment
before it takes A. Since then, on Windows: New Game, Resume Game and Mission
Select from the menus, Seoul to its rooftop and Atlas to its funeral, the
textures put right and the sound checked by what is said in it
("The campaign from its menus", at the end). No level has been played to
its end.

## The two discs

Both images are 8,738,846,720 bytes (XGD3), read with `tools/xdvdfs.py` and
never written to. The XDVDFS partition of each starts at `0x2080000`.

| | Game Disc | Install Disc |
|---|---|---|
| Files | 2,057 entries, 8.05 GB: 1,810 in the root, 95 in `english/`, 150 in `$SystemUpdate/` | 254 entries, 4.72 GB |
| Fastfiles (`.ff`) | 1,714, 2.33 GB: the campaign's levels (`seoul`, `recovery`, `lagos`, `fusion`, `detroit`, `greece`, `betrayal`, `irons_estate`, `crash`, `lab`, `sanfran`, `sanfran_b`, `df_fly`, `df_baghdad`, `captured`, `finale`) each with its `_tr` transient zones, 14 `mp_` map zones (one of them `mp_prison_z`) and `mp_vlobby_room`, the `common*_mp` zones (core, bots, horde, zombie) | `code_post_gfx`, `common`, `ui`, `ui_install` and their patches, in nine languages |
| Image and sound packs (`.pak`) | 103, 4.95 GB | `imagefile1.pak`, `soundfile1.pak` and a `soundfile1` for each language |
| Movies | 83 `.bik`, 0.59 GB (Bink is linked into the executables) | three |
| Executables | `default.xex` (9,134,080 bytes), `default_mp.xex` (10,412,032) | `default.xex` (9,134,080): the installer |
| Content packages | none | `content/0000000000000000/41560914/00000002/41560914051BF3C0` (2,133,819,392 bytes) and `.../4156091407406280` (1,981,456,384) |

The game disc has English only; the other languages named in its title are
on the install disc.

The executables (`tools/xexinfo.py`, `tools/xexdump.py`):

| | `default.xex` | `default_mp.xex` | install disc's `default.xex` |
|---|---|---|---|
| Original name | `default.pe` | `default_mp.pe` | `default.pe` |
| SHA-256 | `a8b1e99c27d6d03a0b0700e6eada1e7d24ee4b194e3c9fa844f16c25f8376a73` | `3ef96359f96ee7994e43de70f56a9c2a019d5b841f11318ca2dffdb717b265f8` | `740982a86be39bda897dc22a1f8d0706e877b3568d6d7912a9115fac82741eb2` |
| Title id | `41560914` (AV-2324) | the same | the same |
| Media id (its last four bytes) | `4FA17C64` | `4FA17C64` | `4469A909` |
| Version, base version | `0000000A`, `0000000A` | the same | `00000007`, `00000007` |
| Image base, size | `82000000`, `2D90000` (47.8 MB) | `82000000`, `2CB0000` (46.9 MB) | as the campaign's |
| Entry point | `8263ED28` | `8213A6D0` | `8263EC08` |
| `.text` | `820D0000`, `634424` bytes | `820E0000`, `7515EC` bytes | |
| Other code sections | `.interpr`, `BINK`, `RADCODE`, `.code` | the same but `.code` | |
| Compression, encryption | raw blocks, retail key | the same | the same |
| Static libraries (XDK 2.0.21256.64) | XGRAPHC, XAPILIB, XBOXKRNL, XAUDIO2, XAPOBA, XNET, XONLINE, XHV2, XPARTY, XMCORE, XGSVCEP, XAUTH, XHTTP, D3D9LTCG; LIBCMT 2.0.11886.4 | the same | the same |
| Imports | `xam.xex`, `xboxkrnl.exe`: 288 functions, 12 of the kernel's variables | 298 functions, 13 variables | |

They are the disc's own builds and no title update: version and base version
are the same. Title updates exist (the research names seventeen); the port is
of the disc, and what an update changes is **open**.

`tools/check_aw_xex.py` accepts the game disc's two files by SHA-256 and
nothing else; the installer's is refused.

## What the second disc is, and how the console has it

**Verified** on Windows and on the Thor (the runs below mount both packages
and list them).

The install disc is not a second partition of game files. It holds an
installer and two content packages. The installer is another build (version
7) of the campaign's own executable, started with `ui_install.ff`, whose
menu calls are in both builds' Lua interface (`InstallXenonDiscContent`,
`IsXenonDiscContentPackageAvailable`, `SetXenonDiscContentInstalled`). It
copies the two packages to the hard disk's content folder, where the disc
already has them by their final names:
`Content\0000000000000000\41560914\00000002\<package>`.

Each package is a read-only STFS container (`PIRS`, content type `00000002`,
marketplace content; `tools/stfs.py` lists and extracts them), named
"Installer Package 01" and "Installer Package 02":

| | Package 01 (`41560914051BF3C0`) | Package 02 (`4156091407406280`) |
|---|---|---|
| Marker | `dlcinstall1.id`, one byte | `dlcinstall2.id` |
| Image packs | `imagefile48.pak` to `imagefile54.pak` and `imagefile61.pak`, 1.31 GB | `imagefile55.pak` to `imagefile60.pak`, `imagefile62.pak`, `imagefile63.pak`, 1.08 GB |
| Movies | 46 `.bik`: the loading movies of `seoul`, `recovery`, `lagos`, `fusion`, `detroit`, `greece`, `betrayal`, `df_baghdad` and their in-level screens | 37 `.bik`: those of `irons_estate`, `crash`, `lab`, `sanfran`, `sanfran_b`, `df_fly`, `captured`, `finale` |

So what the install provides is the campaign's streamed textures and its
loading movies; the levels' fastfiles are on the game disc. The game finds
them as it finds downloadable content: `Content_Init` (the string is at
`82033B88`) enumerates the content packages of the title
(`XamContentCreateEnumerator`, one caller, in the wrapper at `8263CC20`),
mounts each under a drive name (`XamContentCreateEx`, wrapper `8263CB88`;
`dlc1:`, `dlc2:`), lists its root (`NtQueryDirectoryFile`), and knows the two
by the marker `dlcinstall1.id` or `dlcinstall2.id`. Files of the disc itself
are opened as `update:\<name>` and `D:\<name>`, the language's as
`D:\english\<name>`.

What the runtime does for it is an installed-content device, not a second
game root. `runtime/kernel/content.cpp` already mounted a title's downloaded
map packs read-only, straight out of the STFS file (`runtime/kernel/
volume.h`); Advanced Warfare's title id is now one it answers for, and the
packages are looked for where the map packs are: `MW2_DLC_DIR`, the `dlc`
folder beside the game folder, at any depth (so the console's own tree
works). Nothing is extracted. What was missing was the listing: a package's
root opened as a folder, and `NtQueryDirectoryFile` over it
(`runtime/kernel/files.cpp`, this title only: the earlier ones were told
there is no such folder and went on).

Where the two images end up:

- **`setup\setup.cmd -Game aw -Iso <game disc> -InstallIso <install disc>`**
  reads the two packages out of the install disc's image into the git-ignored
  `aw\install` (4 GB, once). The Windows game is told of that folder in its
  `.env` (`MW2_DLC_DIR`); for a phone they are pushed to the app's
  `files/dlc`. **Verified** for both (below).
- **The app's installer** takes the two images one after the other, in
  either order (`runtime/install/install.cpp`): an image with the packages is
  the install disc and they are copied to `files/dlc`; the game disc gives the
  root's files and `english/`. After one the screen asks for the other, and
  the game is not started without both. Playing from the image where it lies
  is not offered for this title. **Verified** on the Thor (below).

## Recompiling

`setup\setup.cmd -Game aw` does it (**verified**: from the two images to a
Windows game in 3 minutes on this PC, with the tools already there, and to
the two packages on the Thor); by hand it is as
[MW3-BRINGUP.md](MW3-BRINGUP.md) has it, with `MW2_TITLE=aw` (or `awmp`),
`config/AW.toml` (`AWMP.toml`) and `aw/tu0/`.

| | campaign | multiplayer |
|---|---|---|
| Functions the recompiler wrote | 51,720 | 50,750 |
| Source files | 207 | 204 |
| Jump tables | 210 (206 from XenonAnalyse, 4 from `tools/intext_switch_tables.py`) | 230 (224 and 6) |
| Switch-case errors, first run | 841 | 874 |
| Function extents added | 42 | 59 |
| Switch-case errors now | 0 | 0 |
| Instructions the recompiler does not know | 415 (stepped over, below) | 415 |
| Imports the runtime has, of those asked for | 227 of 288 | 230 of 298 |

- **The helpers.** The eight register save/restore helpers, `setjmp` and
  `longjmp` were found by `tools/port_hooks.py --version tu0` from MW2's disc
  executable: each matches once, with MW2's size.
- **Exception handlers' data in front of functions.** A function with a
  handler has two words before it (the handler and its scope table). The
  recompiler took each pair for code and made a function of it that ran on
  into the next one's jump tables; 80 switch-case errors stayed whatever
  extents were added. `invalid_instructions` in the configuration names the
  two handler addresses of each executable, found as the words that most
  often stand eight bytes before a function `.pdata` describes.
- **Leaf functions with jump tables** get their extents from
  `tools/fixbounds.py`, as for the other titles.
- **Small switches written as counting branches** (`mtctr; cmpwi; bdzf;
  bdzf`). The recompiler writes each as a `goto` whatever its target, and
  three kinds of function did not compile or were wrong: one whose
  impossible case is the address after its end, a leaf ended at its first
  `blr` with its cases behind it, and (the multiplayer) a leaf reached only by
  another function's closing branch, for which the recompiler wrote
  `// ERROR` and a return. `tools/ctr_branch_extents.py` reads the recompiled
  tree for them and prints the extents; with them in, the tree has none.
- **Jump tables in Bink's own section.** `tools/intext_switch_tables.py`
  looked in `.text` only and for the index right before the load. It now
  looks in every section of code and takes an `addi r12,r12,N` between the
  two. Call of Duty 4's tables come out as they were (**verified**: run on
  both of its executables with their finished tables, it adds none).
- **Twelve instructions the pinned recompiler has no case for**, 415 places
  in each executable: `vminsw` (296), `vsubuwm` (72), `vminuh`, `vmaxuh`
  (12 each), `vsububm`, `vcmpgtsb` (6 each), `lvehx` (4), `vaddsws`,
  `bdnzt` (2 each), `vpkuhus128`, `vpkuwus128`, `mulhd` (1 each). Each is a
  `[[midasm_hook]]` calling `runtime/recomp/ppc_extra_instructions.cpp`, as
  Call of Duty 4's two are, written by `tools/extra_instructions.py` from the
  recompiler's own report. `patches/xenonrecomp-mw2.patch` is untouched.
  **Verified against their definitions**
  (`tests/ppc_extra_instructions_test.cpp`, registered with `ctest` where the
  recompiler's checkout is): each of the twelve and both unpacks, through
  registers laid out as the recompiler keeps them, against a model written
  in the console's terms (a vector as sixteen bytes, its elements
  big-endian) with no host vector instruction in it: cases worked out by
  hand, 20,000 random pairs each with the result register also one of the
  operands, and for the unpacks every one of the 65,536 half floats in each
  place one is read from. All fourteen are as defined; none was changed.
  Run on x86-64 with and without optimisation. The same check is built for
  arm64 (the code the phones run takes other paths through SIMDe) and was
  **not run** on a device. The encodings were read again in the image
  (`vpkuwus128` and `vpkuhus128` as Xenia's tables have them, the registers
  of the VMX128 forms, `bdnzt`'s condition bit, `vupkd3d128`'s kinds 3 and
  5). What the test does not say: whether the 415 places are the only ones
  that need a function (the recompiler's report is taken for that), and
  `vaddsws` does not set the saturation flag, which nothing here reads.
- **Two kinds of `vupkd3d128` the recompiler traps on** (half floats, two
  and four to a vector: four places in the campaign, five in the
  multiplayer). The first level's drawing stopped at one. Hooks of the same
  kind (`PpcVupkd3dHalf2`, `PpcVupkd3dHalf4`), checked with the others: two
  halves are halfwords 6 and 7 and give (x, y, 0, 1), four are halfwords 4
  to 7, and an exponent of all ones is a number, as Xenia's unpack has them.

## The title blocks

`runtime/title.h` has a block for each executable, selected by `AW_TITLE`
(and `AW_TITLE_MP`) beside `MW3_TITLE`, on Call of Duty 4's terms
([COD4-BRINGUP.md](COD4-BRINGUP.md), "Why the build also defines
`MW3_TITLE`"). CMake's `MW2_TITLE` takes `aw` and `awmp`.

The D3D9 library is a later build of Modern Warfare 3's, so the entries were
found from MW3's campaign block: for each function there, the function of
this executable whose instructions' kinds, in order, are nearest, then read.

| Entry | MW3 campaign | AW campaign | AW multiplayer | Found by |
|---|---|---|---|---|
| `T_D3D_Present` | `820A16E8` | `820D9030` | `8210E6E0` | 0.92 of MW3's; the one caller of `VdSwap` in each |
| `T_D3D_ArenaWait` | `8209B5E8` | `820DD7F8` | `82102508` | instruction for instruction; first load `[device+11024]` |
| `T_DEV_PresentInterval` | 13872 | 14000 | 14000 | the flip packet writer (`820D82E8`, MW3's `820A11B8`) reads 11028, this, 12284, 11028, 11069 in MW3's order |
| `T_DEV_ProgressBlock` | 11024 | 11024 | 11024 | the arena wait's first load |
| `T_Com_Error` | `8229D688` | `82417C20` | `82485478` | 0.99 of MW3's; the multiplayer's is 0.79 of the campaign's and leaves by `longjmp` |
| `T_Cbuf_AddText` | `8228CA48` | `82408B70` | `824A2308` | instruction for instruction; the "p<digit> " prefix parse read in the multiplayer's |
| `T_DATA_TimeStampBundlePtr`, `DebugMonitorPtr`, `ModuleHandlePtr` | | `82000874`, `82000970`, `8200089C` | `820008A4`, `82000998`, `82000910` | the import records `000100AD`, `00010059`, `00010193`, each present once |

Located and not entered: the shader hand-over at load
(`T_D3D_PatchVertexFetches` is `820E54C0`, MW3's instruction for
instruction; `T_D3D_InitPixelShader` `820DB2B8` by `tools/port_hooks.py`).
`T_Image_FlushMove` (`825A91F0` in the campaign, `822AEAA8` in the
multiplayer) is entered since the textures were put right (at the end).
Not located: the video sizes (three
functions call `XGetVideoMode` in the campaign), the frame-rate dvar, and
everything of the engine's own.

The guest heap starts at `86000000`: the images end at `84D90000` and
`84CB0000` and their function tables at `85A62C12` and `85BBCAB0`.

## What the runtime needed

- **The kernel's variables.** This D3D9 enters `VdHSIOCalibrationLock`
  around `VdRetrainEDRAM` as the device is made. The record of an imported
  variable was left as the file has it (library and ordinal), and the title
  spun on a critical section at guest address `000101C1`. The ten variables
  these executables import beside the three the earlier titles read
  (`ExEventObjectType`, `ExThreadObjectType`, `ExLoadedCommandLine`,
  `XboxHardwareInfo`, `XboxKrnlVersion`, `VdGlobalDevice`,
  `VdGlobalXamDevice`, `VdGpuClockInMHz`, `VdHSIOCalibrationLock`,
  `KeCertMonitorData`) now get storage, their records read from the XEX
  header's import table (`runtime/kernel/kernel.cpp`, this title only). The
  names of ordinals `1BE` to `1C1` are from memory of the kernel's export
  list and from this one use; `VdHSIOCalibrationLock` fits what the code does
  with it, the others were not read by any run.
- **`NtCreateSemaphore`** took its counts from the wrong registers: the
  console's call has no access mask. XAudio2 makes a semaphore for its worker
  threads, failed with "out of memory", made no mastering voice, and the
  voice library then called through a null pointer. Modern Warfare 3 imports
  the function with the same four arguments (read in its executable), so the
  fix is its too; whether MW3 ever calls it was not looked at.
- **The sound driver's callback** is handed the address of a word holding
  its context, as Xenia hands it: this XAudio2 starts by loading its object
  through its argument. Handed the context itself (which the earlier titles'
  callbacks want, and still get), the mixer waited for ever and no frame was
  submitted. With it: 11,262 frames in a 60 s run, none dropped.
- **`XamUserGetDeviceContext`** fails for a player who is not signed in.
  Answered "device 0" for all four, the campaign took its menus' input from
  the last of them.
- **Imports**, generic: `XMsgStartIORequestEx`, `XAudioGetSpeakerConfig`,
  `XamVoiceIsActiveProcess`, `XamGetLocaleEx`,
  `XamBackgroundDownloadSetMode`, `KeQueryBasePriorityThread`, and the six
  `XAudio*Ducker*` calls. No unimplemented import is reached in the runs
  below.
- **PM4 opcode `0x56`** (`SET_SHADER_CONSTANTS`): this XDK's D3D9 writes a
  draw's constants with it. Taken as `SET_CONSTANT2`, as Xenia takes it.
- **The exposure.** The title works its exposure out on the processor: each
  frame the scene's luminance is drawn down to 16x16 texels of one float
  (`k_32_FLOAT`, two destinations in turn, `192BF000` and `192CF000` in the
  runs here), resolved, and read from the texture's memory. A resolve here
  fills a Vulkan image and guest memory kept its zeroes: a scene taken for
  pitch dark, the exposure opened all the way, the level a white wash
  (`pc-seoul-ingame-overexposed-sheet.png`). That one resolve (a
  single-sampled `k_32_FLOAT` of at most 1,024 texels) is now also copied
  into a host-visible buffer and written, tiled and in the destination's
  byte order, once the GPU has finished the submission it went out in
  (`runtime/gpu/vulkan/renderer_resolve.cpp`, this title only). **Nothing
  waits**: the stall is none, and the title gets the luminance late instead,
  1.0 frames on average over 4,179 write-backs
  (`pc-seoul-ingame-exposure-fixed.log`). The picture is right with it
  (`pc-seoul-ingame-exposure-fixed-sheet.png`), on the Thor too.
- `runtime/gpu/vulkan/renderer_pipelines.cpp` includes `<bit>` for
  `std::rotl`, which this toolchain's other headers no longer brought in.

## What has been seen to work

Evidence is kept outside the repository, in
`C:\Users\solim\mw3-android-work\private-captures\aw\`.

### Windows (llvm-mingw, RTX 5080)

The game disc's files in a folder (7.4 GB), the install disc's two packages
beside it as they come off the disc.

| | State | Evidence |
|---|---|---|
| The campaign starts: logos (Bink), the title screen with "Press START", the attract movie | **verified**, 60 presents a second | `pc-first-boot-sheet.png`, `pc-first-boot.log` |
| Both packages mounted (`dlc1:`, `dlc2:`) and listed | **verified** | "content: mounted dlc1" in every log since |
| The first run's two screens, then the main menu (Campaign, Multiplayer, Exo Survival), the Campaign menu, Intel, back | **verified** from an empty profile with the pad alone: Start, a direction on Brightness, A, a direction each way on Safe Area, A (below) | `pc-first-run-brightness-safe-area-main-menu-sheet.png`, `.log`; the menus themselves `pc-sp-main-menu-campaign-sheet.png` |
| A level: Seoul by `devmap seoul`, its opening movie, the drop pod, the save-device dialog, the pause menu, the Exo Type card | **verified**, 60 presents a second, 54,000 draws a second, no fault | `pc-seoul-ingame-exposure-fixed-sheet.png`, `.log`, `pc-seoul-pause-menu.png` |
| Exposure | **verified** right with the write-back; before and after are `pc-seoul-ingame-overexposed-sheet.png` and `pc-seoul-ingame-exposure-fixed-sheet.png` | |
| Sound | the mixer runs (frames submitted, none dropped); checked since by what is said in it (at the end) | the same logs |
| New Game from the menu, a second level, a save written and loaded | **verified** since (at the end); a level played to its end and a checkpoint within one are **open** | |
| The multiplayer: its main menu and Options | **verified**, 60 presents a second | `pc-mp-main-menu.png`, `pc-mp-main-menu-options-sheet.png`, `.log` |
| A multiplayer map, bots, a system link | **verified**, below ("The multiplayer") | `pc-mp-*` |
| `setup\setup.cmd -Game aw -Build pc` from the two images | **verified**: both trees as expected (207 and 204 files, 210 and 230 tables, 415 stepped), `out\pc-aw` starts and mounts the packages from `aw\install` | |

### Android (AYN Thor, `com.aw.recomp.aw`, built by `setup.ps1 -Game aw -Build android -Suffix .aw`)

The flavour is `aw` (`com.aw.recomp`, a violet reticle around a chevron), the
game code in `com.aw.recomp.aw.game` (aw `c5cef5aeaede`, awmp
`6bc76440aa55`; `config/abi/aw`, `config/abi/awmp`). For the first two rows
the game files were pushed with adb and the packages by setup.

| | State | Evidence |
|---|---|---|
| The campaign starts, mounts both packages, reaches the main menu | **verified**, 119 FPS on the overlay. Once with `profile_SetHasEverPlayed_MainMenu` from `options.txt`; then again with no `options.txt` and no profile, by the touch controls alone: Start, Brightness, Safe Area, main menu | `thor-main-menu.png`, `thor-main-menu.log`; `thor-brightness-no-options.png`, `thor-main-menu-by-touch-no-options.png` |
| Seoul by `devmap seoul` | **verified**: in the drop pod at 119 FPS, exposure right (the write-back is logged) | `thor-seoul-level.png`, `thor-seoul-level.log` |
| The app's installer, with both images in the device's Download folder and nothing installed | **verified**: Install & Play copies the game disc (1,809 files and the 95 of `english/`, 7.3 GB, about a minute), the screen asks for the second disc, the install disc's image is chosen in the app's file browser, the two packages are copied to `files/dlc` (4 GB, under a minute) and the game starts and mounts them. The install disc first was **not run** | `thor-installer-asks-for-second-disc.png`, `thor-after-in-app-install-both-discs.png` |
| The multiplayer, the Flip 2, a release build | **not run** | |

The app gives this title the other games' start-up commands (`com_maxfps 0`,
`r_vsync 0`, `sm_enable 0`, `cg_fov 85`); what each does here was not looked
at. The app carries the build kit for this title (the game code built on the
device, [ondevice-build.md](ondevice-build.md)): `InTextTables.kt` now does
what the script does (every section of code, the `addi` form) and writes
the script's tables to the byte for both of this title's executables and
both of Call of Duty 4's (**verified** on this PC, in a unit test against
the script's output); the build itself was **not run** on a device.
`setup/setup.sh` (Linux) knows this title since (`--game aw`,
`--install-iso`); it was **not run** from end to end.

### The other titles

**Checked** on this branch against `origin/android`, before the Android
work: for mw3, mw3mp, mw2, mw2mp, cod4 and cod4mp the analyser's jump tables
hash to what `config/abi/` records, `tools/intext_switch_tables.py` adds the
same tables to Call of Duty 4's as `android`'s does, and `tools/fixbounds.py`
prints the same extents as `android`'s, byte for byte (26, 32, 11, 22, 34,
36). No input of their game-library statements differs: the recompiler
patch, `tools/genshared.py`, `runtime/mmio_hook.h`, their configurations and
`config/abi/` are untouched, and `setup/setup.ps1` still names the same
recompiler commit. Call of Duty 4's game code built from this branch states
what the published app expects (`cod4 94ec5f58a992`, `cod4mp a4c71519c558`).

**Built and run** from this branch, on Windows, each from an empty profile
for 50 to 95 s at 60 presents a second with no unimplemented import reached
(`pc-regress-six-titles-from-aw.png`, `pc-regress-mw3-mw2-campaign-menus.png`):

| | Reached |
|---|---|
| mw3 | its setup screens, the main menu, the CAMPAIGN menu |
| mw3mp | the multiplayer menu and its "no network connection" notice |
| mw2 | Brightness, the margins, the content notice (the main menu is one answer further; not pressed) |
| mw2mp | the multiplayer menu and its sign-in notice |
| cod4 | the main menu, New Game, F.N.G. in play |
| cod4mp | the Xbox LIVE menu |

With the same input, the builds made before this branch (`out\pc\mw3.exe`,
the cod4 branch's MW2) stop where these do.

What they share that changed, and why it does not reach them:

- `NtCreateSemaphore`: only Modern Warfare 3's executables import it; both
  ran (above). Its arguments were read in MW3's executable.
- `XamUserGetDeviceContext`, `NtQueryDirectoryFile` over a package's root,
  the sound callback's context, the kernel's variables, the luminance
  write-back: none of the six imports the first, and the rest are compiled
  for `AW_TITLE` alone.
- PM4 `0x56`: none of the six runs' streams has the packet (their reports
  count every opcode).
- The installer's copy loop (`runtime/install/install.cpp`, Android only)
  and `launcher/disc.cpp`: Call of Duty 4's app built from this branch
  (`com.cod4.recomp.aw`) copy-installed that game on the Thor from its disc
  image, 115 files and the 2,480 of `highmip/`, 6.4 GB, and started
  (`thor-cod4-from-aw-after-copy-install.png`). Modern Warfare 2's and 3's
  apps were **not** built from it; they take the same path without a
  folder of the disc's.
- `SubmissionFinished` is the occlusion queries' check under a name of its
  own; Call of Duty 4, the one title with queries, ran on both.

## The first run's screens

With no profile, Start opens Brightness (`boot_brightness`) and not the main
menu, and Brightness was taken for dead: A, B, X and Start do nothing on it
and it shows no prompt. It is not dead. The menus are Lua, and Lua asks the
engine whether the dvar `lui_print_events` is set; with `set lui_print_events
1` from the console its own trace of events came out through the
interpreter's string conversion (`8213E240`, hooked for the investigation and
not kept). It shows the pad's events arriving (`gamepad_button`, "button
primary is down") for controller 0, and that the screen puts up "Press A to
continue" (`continue_message_visible`) only once the brightness has been
changed (`brightness_updated`), by left or right; A then goes on to Safe Area
Adjustment (`boot_screen_margins`), which went on to the main menu after one
adjustment each way and A. Nothing in the runtime was wrong or changed for
it, and the console command the earlier runs used is not needed. It is the
series' way: Modern Warfare 3's setup screens show their `A` prompt only
after the slider has been moved too
([MW3-LOCAL-RUN.md](MW3-LOCAL-RUN.md), `ui_brightness_adjusted`).

The controller's binding, its capabilities, the sign-in state and the
profile's settings were all looked at on the way and are as the title wants
them: controller 0 is the first active one, its profile is signed in, and
`profileMenuOption_blacklevel` exists.

## Open, in the order it matters

1. A level played to its end and the next one started by it; a checkpoint
   within a level, written and resumed. Neither has been seen.
2. Everything on the Retroid Pocket Flip 2: the campaign's flow, its frame
   rate, the stepped-over instructions' check on arm64, and the build kit
   run on a device (it is in the app; `InTextTables.kt` was checked against
   the script on a PC, the build itself was not run).
3. The multiplayer's Xbox LIVE menu, Exo Survival and Combat Readiness
   Program ("The multiplayer", below, has what works).
4. `setup/setup.sh` for this title from end to end on Linux.

## After merging `android` in (24 November 2026)

`origin/android` (`86a1ef1`, v0.1.7) was merged into this branch. On the
merged tree, **verified**: `setup.ps1 -Game aw -Build both -Suffix .aw`
builds both; the Windows campaign runs at 60 presents a second; the Thor
reaches the main menu by touch at 119 FPS; the six other titles' jump
tables, in-text tables and extents are still what `config/abi/` and
`origin/android` give; the 17 standalone tests (`-DMW2_TESTS_ONLY=ON`,
Ubuntu under WSL) pass, and so do the validation workflow's tool checks.
The other titles were **not** rebuilt after the merge.

## The campaign from its menus (24 November 2026, Windows)

Driven by `MW2_INPUT_SCRIPT` from an empty profile; evidence in
`private-captures\aw\` as before. The PC had no sound device during these
runs, so the sound is the mix as the title submitted it
(`MW2_AUDIO_DUMP`), not what a speaker played.

| | State | Evidence |
|---|---|---|
| New Game: Campaign, New Game, the autosave notice, Select Difficulty, Seoul's loading movie | **verified** | `pc-newgame-from-menu-sheet.png`, `.log` |
| Seoul: the Exo Type card, the drop pod, "Tap X to Pull Lever", "LT or RT to Release Harness", "RS to Open Hatch", the rooftop with the weapon in hand and the objective marker | **verified**; no further: the player was turned and walked by timed input and did not find the way down | `pc-seoul-resume-pod-rooftop-textures-fixed-sheet.png`, `.log` |
| A save: `saves/savegame.svg/savegame.svg`, 1,181,466 bytes, its header naming `seoul` and "Start Level Save" | **verified** written at the level's start | the log's `content: mounted save0:` lines |
| Resume Game: "Resume Game?", the save opened, the level from its start | **verified** for Seoul and for Atlas | `pc-resume-game-dialog-sheet.png` |
| A checkpoint within a level | **open**: the save did not change in any run, and none got far enough to say whether it should have | |
| Mission Select: the first mission's card and the row of missions; the later ones dimmed and not selectable on a profile that has only started Seoul | **verified** | `pc-mission-select.png`, `pc-mission-select-locked-without-mis-cheat.png` |
| Atlas (`recovery`), chosen in Mission Select with the title's own `set mis_cheat 1` from the console: the warning that the mission's progress is overwritten (No is the default), Select Difficulty, its loading movie, the Exo Type card, the funeral, "X To Pay Respects"; its start save written (785,421 bytes) | **verified**; no further | `pc-atlas-from-mission-select-sheet.png`, `.log` |
| Sound: the movies' narration and the levels' speech | **verified** by transcribing the dump: Seoul's "We come into this world with our eyes closed", the pod's countdown and "Mitchell, now!", the rooftop's orders; Atlas's eulogy. In a level the speech is in the front pair and the centre is silent | `pc-seoul-audio-transcript.txt`, `pc-atlas-audio-transcript.txt` |
| A level's end and the next level from it | **open** | |

**The textures.** From Seoul's first minute buildings, faces and the pod were
drawn as coloured noise, different in every run
(`pc-seoul-textures-noise-before-image-move-sheet.png`). The textures read
from guest memory were noise themselves (`MW2_DUMP_TEXTURES`), some in
whole and some in quarters, and reading them again at every use changed
nothing, so the memory held it. The title keeps its streamed images in a
pool it compacts (`sub_822C8628` loads them, `sub_825A9A40` allocates), and
moves a block by having the GPU copy it: a vertex shader's memory export,
which this renderer does not do. The earlier titles' hook makes the copy at
its draw (`runtime/image_move.cpp`); this title's function was located and
not entered. Entered (`T_Image_FlushMove`, its arguments and the copy
draw's stream constant read in the function), the picture is right. The
pool is busy: 144,279 blocks moved in 400 s of Seoul, and the texture
report counts a million signings after its copies, whose cost on a phone
was **not measured**.

**Seen and not explained.** One run of three with the same build and input
spent 75 s of the pod below 30 presents a second; the PC was shared with
other builds at the time. Timed input is also why two runs stopped where
they did: a prompt came after the presses meant for it had ended.

## The multiplayer

State on 24 November 2026: a System Link match is hosted with the title's
own bots on Windows, the AYN Thor and the Retroid Pocket Flip 2, and joined
by another copy over the local network and through the relay
(`mw3.yassin.app:3074`): two copies on Windows, and the Thor and the Flip 2
in one match, each hosting in turn. On Windows two copies also meet by Xbox
LIVE's Find Match and by an invitation to a private match, both through the
relay, and Exo Survival is played by one copy and by two ("Xbox LIVE and Exo
Survival", below). Neither was run on a device; Combat Readiness Program is
open.

### What the executable has (derived, from `default_mp.pe`)

Strings and the code that loads their addresses, read with capstone over
`tools/title.py`'s image.

| What | Address | Found by |
|---|---|---|
| `SV_AddBot(name)` | `824120C0` | one of two users of `connect bot%d "%s\name\%s\qport\%d\ctz\%s"` (`8208F0B0`); returns the bot's gentity |
| `SV_AddTestClient` | `824123F0` | the other; names its client `tcBot%d` |
| `SV_DirectConnect` | `82411388` | both call it with the address by value in `r3:r4` (type 0, the bot's port); `error\nEXE_SERVERISFULL` is its |
| the server's argv tokeniser | `824A2830` | `(text, 512 - pool[0x4820], args 838229A0, pool 83819950)`, called before it, as Call of Duty 4's |
| `NET_CompareAdr` | `82479FF0` | compares a client's address (+0x28, 16 bytes) with the new one |
| `svs` | `83D48780` | `SV_AddBot`'s `lis/addi`; +0x207E8C the client count, +0x207E90 the clients, +0x2134D0 the next bot's port |
| a server client | 0x77A80 bytes | the loop's step; +0 the state (0 free), +0x28 the address, +0x31B04 set to 2 for a bot |
| a gentity | 640 bytes | `SV_AddBot`'s return: `[83F5C680 + 0xC73C] + number * 640` |
| the match rules | `82247CE0(which, reset)`, blocks of 0x28 bytes at `84779560` | every bot setting of the menus goes through it |
| the bots' defaults | `82247A98` | the leaf that fills +0x20..+0x24 from `bot_AutoConnectDefault` and `bot_DifficultyDefault` |
| the Lobby menu functions | table at `82060180`.. | `SetBotsTeamLimit` `82717800`, `SetBotsDifficulty` `82717AF0`, `SetBotsConnectType` `82718110`, `BotsAreAllowed` `82714A80`; `RemoveMemberBot` (`827168E0`) reads its arguments and does nothing on this console |
| `luaL_error` | `827636B8` | 1160 callers, all in the Lua library |
| the commands of `CL_Init` | `82601830` | `xstartlobby`, `xstartprivateparty`, `xstartprivatematch`, `xpartygo`, `connect`, `disconnect`, `startSingleplayer`; the dvars `systemlink` (`82B0CE38`), `onlinegame` (`839B200C`), `xblive_privatematch` (`84215734`) |
| the item check | `82495080(controller, stats, table, row, int* needed)`, through `82496258` | the users of `mp/unlocktable.csv`; 0 is unlocked |
| the image pool's move | `822AEAA8` | the campaign's, laid beside it (found by the campaign's work) |
| the server's dvars | registered in `8240A938` | the one user of the names `sv_timeout` (`8208F288`, default 10, 0 to 1800) and `sv_connectTimeout` (`8208F294`, default 40); the pointers are kept at `83D48738` and `83D48734` |
| `SV_CheckTimeouts` | `82405470` | the only other user of those two pointers: each dvar's number (+12) times 1000 against the time of a client's last packet; states 2 and 5 against `sv_timeout`, the others above 1 against `sv_connectTimeout` |
| the client's and the party's patience | `82601830`, `825DEC30` | `cl_timeout` 10.0 and `cl_connectTimeout` 60.0 (floats); `pt_memberTimeout` 25000 ms |

Only the bots' defaults, the match rules' address, `luaL_error`, the image
pool's move and the server's dvars are entries of `runtime/title.h`: nothing
else is called.
The usercmd and the client states were not needed and not derived: the bots
think in the engine.

**Why `devmap` does nothing here.** The multiplayer's executable has no
such command: the strings `devmap`, `fast_restart` and `spdevmap` are in the
campaign's and not in it. A map starts from a lobby: Create Game, then
Start Game (the party's `go <map> <game type>` to every member).

### What was changed

- **The network** (`kernel/net.cpp`, `online/service.cpp`). The menu asks for
  the Ethernet link before System Link or Xbox LIVE opens; it is up when an
  online service started, as for Modern Warfare 3. The title's two sockets
  (port 1000, the voice and data protocol, and port 1001, UDP) are the
  service's; with none they stay on the loopback. On a relay the copy is
  signed in to Live (`MW2_LIVE`, as Modern Warfare 3's): see "Xbox LIVE and
  Exo Survival".
- **Bots** (`runtime/aw_bots.cpp`). `MW2_BOTS`, `MW2_BOT_SKILL`,
  `MW2_BOT_TEAMS` and the console's `bots` line write the lobby's own bot
  rules (the smaller half friendly, the rest enemy, connect type
  `fill_open`); the scripts of `common_bots_mp.ff` add the bots, which walk
  the maps' path data, boost-jump and use their classes. `runtime/bots.cpp`
  is not used. Game Setup's Friendly Bots and Enemy Bots still decide when
  the player sets them. At most 11.
- **The streamed images' move** (`T_Image_FlushMove`): without it most
  world and weapon textures were noise.
- **The host's patience** (`runtime/aw_server.cpp`). A host built from this
  source waits for a silent player in its match as long as the title waits
  for one connecting: `sv_timeout` is given `sv_connectTimeout`'s 40 seconds
  as the server's dvars are registered. Why is below ("Why a join was lost").
- **Unlocks** (`runtime/aw_unlock_all.cpp`, `MW2_UNLOCK_ALL`, which the app
  sets). In System Link, Create a Class lists every weapon
  and all seventeen attachments of a rank 1 profile without a padlock
  (`pc-mp` runs, 24 November), so the hook on the item check has nothing to
  do there. On the Flip 2, from a profile
  the app had just made (rank 1), Create a Class shows ten custom classes,
  each already filled (ASM1 with three attachments, four perks, Exo Stim,
  Semtex, a UAV and two wildcards) under a Pick 13 bar at 13/13, and
  Primary Weapons lists its submachine guns (KF5, MP11, ASM1, SN6, SAC3,
  AMR9) with no padlock (`flip-mp-pick13-*`). Under Xbox LIVE the check
  applies: without the switch a rank 1 player's Primary Weapons has a
  padlock on all but the first assault rifle
  (`pc-mp-findmatch-relay-copy-b-sheet.png`). The hook is back in for that;
  **it was not run under Live after it went back in.**

### Seen on Windows (RTX 5080, two copies on one PC, `MW2_NETWORK=lan`)

Driven by `MW2_INPUT_SCRIPT`: Down, Down, A (System Link), Down, A (Create
Game), A (Start Game), A (Auto-Assign), A (the first class). The map was
chosen with `set ui_mapname <map>` from the console before Start Game, which
loads that map but leaves the lobby naming the other one: a second copy
then cannot join ("Unable to join game session"), so the joins below are on
the lobby's own map. Evidence in `private-captures\aw\`.

| | Seen | Evidence |
|---|---|---|
| Team Deathmatch on Ascend (`mp_refraction`), `MW2_BOTS=6`, 5 minutes | three bots a side, 16 to 19 at 5:01; 60 presents a second, 56.3 frames a second in play over the run with its loading | `pc-mp-ascend-6-bots-scoreboard.png`, `-sheet.png`, `.log` |
| The same on Bio Lab (`mp_lab2`) | 22 to 13 at 5:11; 57.4 | `pc-mp-biolab-6-bots-*` |
| Detroit, Retreat | loaded and played; `bots 9` from the console in a match brought the bots to nine | not kept |
| A second copy joins a lobby | "Waiting for host to start game", both listed | not kept |
| A second copy joins a match with `MW2_BOTS=11` | the server had ten bots (four friendly, six enemy: the scripts keep the host's place in his team's six) and the player took the twelfth slot; both scoreboards list him | `pc-mp-leave-rule-*` |
| The same through the relay (`mw3.yassin.app:3074`), before the host's patience | the game listed, the session joined, the level's loading screen, then "Server Disconnected" in one run; the same join held in the next | `pc-mp-relay-*`, `pc-mp-relay2-*` |

### Why a join was lost, and what holds it now

It was not the relay's. The packets of a join are the same over the local
network and through the relay (1264 bytes at most, far below the relay's
2048; the same ports and addresses; nothing dropped by a limit), and the same
join through the relay held or failed from one run to the next.

What differed was how long the joining copy said nothing. Its first frames of
a level translate and compile every shader the level draws with, on the
thread that consumes the title's command stream (Advanced Warfare hands no
shader over at load: "193 never handed over"), and the title's main thread
waits for that thread (`82143130 <- 8227DEFC <- 822AC018 <- 8263800C`, the
watchdog's dump at the stall; the ring 324 words behind). On this PC that is
6 to 11.5 seconds with no packet sent. The host's `SV_CheckTimeouts` drops a
player in the match after `sv_timeout`, 10 seconds: its log shows the last
packet from the joiner at 162.8 s and its `endparty` and `disconnect` at
173.2 s.

Seen with two copies through the real relay, the joiner's process suspended
for a few seconds as the level starts (`NtSuspendProcess`, under the relay's
own 10 seconds so that it is not forgotten) to make the silence a known
length:

| Host | Joiner silent for | Result | Evidence |
|---|---|---|---|
| as the title, `sv_timeout` 10 | 10.7 s | "Server Disconnected", 10.3 s after its last packet | `pc-mp-relay-stall15-*` |
| patient (40) | 9.3 s | in the match, both scoreboards list both | `pc-mp-relay-fixed-second-*` |
| patient (40) | 13.6 s | in the match | `pc-mp-relay-fixed-silent14-*` |
| patient (40) | 20.6 s, 26.6 s | the host keeps the player; the joiner itself gives up ("Server timed out", `82600D80`) | `pc-mp-relay-fixed-silent21-*`, `-silent27-*` |

The last line is the limit that is left. The title sends what a joining
player must have reliably, and doubles the wait before each repeat (0.1 s to
12.8 s seen), so after a long silence the host's next packet is many seconds
away and the joiner's own `cl_timeout`, 10 seconds, ends first. Nothing was
changed for it: no copy has been silent that long by itself, and the remedy
is the shaders compiled before the level draws, as for the other titles.

**The relay needs no change**: nothing to deploy.

### Seen on the devices (24 November 2026)

The app built with `-Suffix .awm` (`com.aw.recomp.awm`), the game disc's
files and the install disc's packages in its own folder, launcher settings
left as they start (the display's rate, 1024x600 scene) but for Bots at 6.
By touch: System Link, Create Game, Start Game, Auto-Assign, Custom 1; the
second map from Game Setup, Maps. The frame rates are the app's own
counter, read from the screenshots; the runs were ended by stopping the app,
so there is no end-of-run report.

| Device | Match | Seen | FPS | Evidence |
|---|---|---|---|---|
| Flip 2 (Adreno 650, 60 Hz) | Retreat, Team Deathmatch, 6 bots, 6 minutes | three bots a side, 23 to 25 with 2:13 left | 60 to 61 | `flip-mp-retreat-6-bots.png` |
| Flip 2 | Comeback, the same, 6.5 minutes | 30 to 22 with 2:46 left | 60 to 61 | `flip-mp-comeback-6-bots.png` |
| Thor (Adreno 740, 120 Hz) | Retreat, the same, 6 minutes | 22 to 23 with 3:28 left | 119 | `thor-mp-retreat-6-bots.png` |
| Thor | Detroit, the same, 6.7 minutes | 22 to 35 with 2:38 left | 119 | `thor-mp-detroit-6-bots.png` |
| Thor hosts, the Flip 2 joins through the relay | Retreat with the host's 6 bots; the Flip 2 lists the game (`thor`, Retreat, Team Deathmatch) and joins the match under way | both scoreboards list `thor`, `flip` and the six bots | 119 / 59 | `relay-thor-hosts-flip-joined-*.png`, `thor-mp-relay-game.log` |
| Flip 2 hosts, the Thor joins through the relay | the same | the same | 119 / 60 | `relay-flip-hosts-thor-joined-*.png`, `flip-mp-relay-game.log` |

The two relay matches ran with the host's patience in the app (the log's
`server:` line); neither device was silent long enough to need it there:
both had drawn the map before. A Windows copy also joined the Flip 2's
lobby through the relay ("Waiting for host to start game").

**Open:** whether a bot leaves when a third player needs its place (the
scripts' `fill_open`, not seen with a full team); a join by a copy that is
silent for twenty seconds or more (above); Combat Readiness Program (not
opened).

### Xbox LIVE and Exo Survival (24 November 2026, Windows)

**What the title asks, and what answers.** Signed in to Live, choosing Xbox
LIVE logs on to the publisher's DemonWare servers, in this order (the state
machine at `822F5388`, one state a step):

| Step | The title's | Answered by |
|---|---|---|
| Which Live environment | `XamQueryLiveHiveA("activeservices")`, searched for `.part.xboxlive.com` and `.cert.xboxlive.com` (`822F4B68`) | `kernel/xam_misc.cpp`: empty, the retail one |
| The logon | one HTTPS POST of JSON to `aw-xbox360-auth3.prod.demonware.net/auth/` through XHttp (`bdXHTTP`, the thread at `821BAA18`; `bdAuthXenon` `821C6310`, the reply read at `821C6718`), after asking Live for a token (`XamGetToken`) | `kernel/xhttp.cpp`, a request answered in the process; `online/demonware.cpp` writes the reply: task 0x2B, code 700, a client ticket the title takes undecrypted because its magic number reads as it is (`821C6A2C`), and `{"dpi": ...}` |
| Where the lobby is | `XOnlineGetServiceInfo(0x415607E9)`, the title server `S1 - DW ATVI LSP` (`822F5190`) | as Modern Warfare 3's, `kernel/xam.cpp` |
| The lobby | one TCP stream to port 1027, Modern Warfare 3's protocol, not encrypted | `online/demonware.cpp` |
| The playlists | `playlists.aggr` (storage task 7), then `social.cfg`, `mm.cfg`, the message of the day | `relay/playlists_aw.info` unless the relay has a file; the others as Modern Warfare 3's or "no such file" |
| The player's files | `metplayerdata` and `mpdata` (storage tasks 12, 10, 13: get `821C42A0`, upload `821C4128`, list `821C43E0`, each with a context name first) | kept under `saves/online/user/<xuid>/` |
| A public match | matchmaking, service 138: create 1 (`821AF470`), update 2, delete 3, find 16 (`821AF978`); **not** `XSessionSearch` | the online service's listings (`online/demonware.cpp`, "bdMatchMaking") |
| The line to a host | `XNetQosLookup` | from the search, as for the other titles |

A session the title lists is the console's (the host's XNADDR, id and key),
a game type (2 for the multiplayer, 5 for Exo Survival: the table at
`820598E0`), the slots, and fifteen words that mirror the contexts and
properties it sets (playlist, protocol, map packs, open slots, where the
host is). A relay's listing is 60 bytes and four counts, so the id, the key
and eight words are the bytes, the slots the counts, and the host's address
is rebuilt from who listed it. A search finds sessions of the same playlist,
rule, protocol and map packs and of the searcher's own game type, with at
least the open slots it asks. **The relay needs no change and has nothing to
deploy**: it has no playlists for this title, so each copy uses the file its
release was built with.

Not answered: every other task (the marketplace's, counters, groups, stats,
content streaming) succeeds with no results; dedicated servers (task 5).

**Seen**, two copies on one PC through `mw3.yassin.app`, driven by
`MW2_INPUT_SCRIPT`; evidence in `private-captures\aw\`:

| | Seen | Evidence |
|---|---|---|
| Find Match | Team Deathmatch and Free-for-all listed; both copies choose Team Deathmatch six seconds apart; the second finds the first's lobby (`dw: the search found 1 sessions`), "Match beginning in", Retreat loads, both scoreboards list `pcA` and `pcB` on opposite teams with the same clock | `pc-mp-findmatch-relay-copy-*` |
| Private Match by invitation | the host opens Private Match and the friends screen (the title's own `xshowfriendslist`; F6 or the app's pause menu do the same), which invites everyone heard; the other copy (`MW2_LAN_ACCEPT=1`) accepts from the Xbox LIVE menu, the host starts, both play Retreat | `pc-mp-private-invite-copy-*` |
| Private Match by "join session" (`MW2_LAN_JOIN`) | refused by the title: "This friend's game or party session is private; you can only join from an invitation message" | not kept |
| The title's own Friends screen | "You have no Xbox Live friends": the friends enumeration (`XFriendsCreateEnumerator`, message `00058020`, `82194348`) is answered empty | not kept |

**Exo Survival** is the same executable in another mode. The mode is a word
of the launch data (`XamLoaderGetLaunchData`, 0x3F8 bytes read at
`823522C0`: the first word 11, the mode at +0x3D8; 4 is Exo Survival in the
table at `820598E0`, and no mode is the multiplayer, `82487968`). The
campaign's main menu writes it and starts this executable; the app and the
Windows build carry launch data across already. `StartSurvival` and the
`startSurvival` command do nothing in this executable. Started with that
launch data (`MW2_LAUNCH_DATA`, hexadecimal) the menu is EXO SURVIVAL
(CO-OP) with Xbox Live, Local Play and System Link.

| | Seen | Evidence |
|---|---|---|
| One player, System Link, Bio Lab | the Exo choice, "Survive", round 1's enemies, one kill for 100 points, the player's death and the After Action Report | `pc-survival-solo-round1-*` |
| Two players, System Link through the relay | the second copy joins the lobby, both choose an Exo and play round 1; "Survivors lost" lists both | `pc-survival-relay-two-players-copy-*` |

**Not seen:** a round survived (the player is driven by timed input and
shoots at nothing in particular); Exo Survival chosen in the campaign's own
menu and handed over (the launch data was written by hand); Exo Survival
under Xbox LIVE; any of this section on a device.
