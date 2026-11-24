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
recompile clean, and the campaign's Windows build draws its logos, its title
screen and, after Start, the first-run Brightness screen at 60 frames a
second; it does not leave that screen, so the main menu is **not** reached.
The multiplayer's Windows build links and has not been run. Android has not
been started.

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

**Derived**, from the disc and from the campaign's executable; the mount was
not reached in any run.

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
mounts each under a drive name (`XamContentCreateEx`, wrapper `8263CB88`),
and knows the two by the marker it then opens as `<drive>:\<name>.id`, with
the names `dlcinstall1` and `dlcinstall2` (`82033A0C`, `820339B4` to
`820339DC`). Files of the disc itself are opened as `update:\<name>` and
`D:\<name>` (**verified** in the runs below: `update:\code_post_gfx.ff`,
`update:\imagefile1.pak`, `D:\shg_logo.bik`).

What the runtime needs for it is therefore an installed-content device, not a
second game root: an enumerator that answers for content type 2 with the two
packages, and `XamContentCreateEx` mounting a package's folder read-only
under the name asked for. **Open: not written.** `runtime/kernel/content.cpp`
mounts saved games only (a folder under `saves/`), and
`XamContentCreateEnumerator` answers with nothing. The packages are extracted
on this PC as folders (`D:\aw-data\install\<package>`, 2.0 and 1.9 GB) ready
for it. Setup and the app's installer do not know the second disc yet either.

## Recompiling

By hand, as [MW3-BRINGUP.md](MW3-BRINGUP.md) has it, with `MW2_TITLE=aw` (or
`awmp`), `config/AW.toml` (`AWMP.toml`) and `aw/tu0/`. `setup\setup.cmd` has
no `-Game aw` yet (**open**).

| | campaign | multiplayer |
|---|---|---|
| Functions the recompiler wrote | 51,720 | 50,750 |
| Source files | 207 | 204 |
| Jump tables | 210 (206 from XenonAnalyse, 4 from `tools/intext_switch_tables.py`) | 230 (224 and 6) |
| Switch-case errors, first run | 841 | 874 |
| Function extents added | 42 | 59 |
| Switch-case errors now | 0 | 0 |
| Instructions the recompiler does not know | 415 (stepped over, below) | 415 |
| Imports the runtime has, of those asked for | 220 of 288 | not counted again since the last additions |

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
  **Not verified one by one**: no test compares them with the console's
  results, and which of them the runs below go through was not looked at.

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
instruction; `T_D3D_InitPixelShader` `820DB2B8` by `tools/port_hooks.py`) and
`T_Image_FlushMove` (`825A91F0`, 0.85). Not located: the video sizes (three
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
- **Seven imports**, generic: `XMsgStartIORequestEx`,
  `XAudioGetSpeakerConfig`, `XamVoiceIsActiveProcess`, `XamGetLocaleEx`,
  `XamUserGetDeviceContext`, `XamBackgroundDownloadSetMode`,
  `KeQueryBasePriorityThread`. With them no unimplemented import is reached
  up to the Brightness screen.
- `runtime/gpu/vulkan/renderer_pipelines.cpp` includes `<bit>` for
  `std::rotl`, which this toolchain's other headers no longer brought in.

## What has been seen to work

Evidence is kept outside the repository, in
`C:\Users\solim\mw3-android-work\private-captures\aw\`.

### Windows (llvm-mingw, RTX 5080; built by hand from `out/build-pc-aw`)

The game disc's files copied to a folder (7.4 GB); the install disc's content
not mounted.

| | State | Evidence |
|---|---|---|
| The campaign starts: D3D9 makes its device, the sound and voice libraries start, the fastfiles `code_post_gfx`, `ui`, `common` and their patches and `imagefile1.pak` are opened | **verified** | `pc-first-boot.log` |
| Sledgehammer, Activision and High Moon logos (Bink), the title's logo, the title screen with "Press START", then the attract movie | **verified**, 60 presents a second | `pc-first-boot-sheet.png` |
| Start: the first-run Brightness screen | **verified** | `pc-title-then-brightness-sheet.png`, `pc-title-then-brightness.log` |
| Leaving the Brightness screen; the main menu | **open: blocked.** A, B, X, Start and right, pressed by `MW2_INPUT_SCRIPT`, change nothing, and the screen's row of button prompts is not drawn | the same sheet |
| Sound | **open**: the render driver's client is registered and called once; no frame is submitted in 60 s | "audio: 1 callbacks, 0 frames submitted" in both logs |
| Exposure (the luminance read-back) | **open**: no level was reached, so nothing was measured or changed | |
| The multiplayer | builds and links (`out/build-pc-awmp`); **not run** | |

No guest fault, no unknown GPU opcode and no unimplemented import in the
last run.

### Android

Not started: no `aw` flavour, no `.aw` build, no device slot asked for.

### The other titles

**Not checked** against `android`: the game-library statements and the
`fixbounds` output of mw3, mw3mp, mw2, mw2mp, cod4 and cod4mp were not
compared on this branch, and none of them was built from it. What this
branch changes that they share: `CMakeLists.txt` (new branches for `aw`),
`runtime/title.h` (two blocks in front), `runtime/guest_memory.h`,
`tools/title.py`, `tools/intext_switch_tables.py` (checked for Call of Duty
4, above), `NtCreateSemaphore`, the seven imports, `<bit>`, and
`mw2_guest_base` in `runtime/guest_memory.cpp`. No input of a game-library
statement was edited (`setup/`, `patches/`, `tools/genshared.py`,
`runtime/mmio_hook.h`, the other titles' configurations). The branch is not
merged.

## Open, in the order it blocks

1. **The Brightness screen takes no input.** The two leads, neither followed:
   the sound driver's callback stops after one call, and the screen's button
   prompts are missing, so the menu may be waiting on something that never
   completes rather than ignoring the pad (Start is taken on the screen
   before).
2. **The installed-content device** (above), then setup and the app's
   installer reading both discs.
3. The multiplayer's first run.
4. A level, and with it the exposure read-back: a write-back of the luminance
   resolve alone, measured, with pictures before and after.
5. `setup.ps1 -Game aw`, the Gradle flavour `aw` (`com.aw.recomp`, an emblem
   of its own), `config/abi/aw*`, the Android build and a device.
6. The comparison with the other titles, before any merge.
