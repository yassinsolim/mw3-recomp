# Downloadable content (map packs)

**The short answer for a player.** The games on the discs are complete and
this project ships no map packs, and never will: they are sold content. If
you bought a map pack on your own Xbox 360, the port can read **your own
package file from your console's hard disk**: put it in a `dlc` folder and
the multiplayer mounts it as the console did.

- **Advanced Warfare: it works, with a player's real packs.** The four map
  packs' maps and Exo Zombies were played from the owner's own packages
  ([Advanced Warfare](#advanced-warfare-map-packs-and-exo-zombies)). They
  need the game's **title update** as well, which is a file on the same hard
  disk, and the multiplayer built a second time from it.
- **Modern Warfare 2, Modern Warfare 3 and Call of Duty 4: experimental.**
  The path is written and tested with a synthetic package made from a disc's
  own files; it has not been tried with a real map pack of these games,
  because the project owns none, and a real pack was made for a later game
  version than the disc's, which the port plays
  ([what is untested](#what-is-untested)).

## What a player supplies

A map pack on the console is one file: an STFS package (it begins with `LIVE`,
or `PIRS`), with no extension and a name of forty characters, kept on the hard
disk under

    Content\0000000000000000\<title id>\00000002\<package>

| game | title id | drive the title mounts | marker inside |
|---|---|---|---|
| Modern Warfare 2 | `41560817` | `mappack1:` ... | `mappack1.id` to `mappack6.id` |
| Modern Warfare 3 | `415608CB` | `dlc1:` ... | `dlc1.id` ... |
| Call of Duty 4 | `415607E6` | `mappack1:` ... | (read by the title) |
| Advanced Warfare | `41560914` | `dlc1:` ... | `dlc1.id` to `dlc5.id`, `dlccompat1.id` to `dlccompat6.id`, `seasonpass.id` |

Copy the package off your own console's disk (a USB stick formatted by the
console and a tool that reads its `Content` folder, or the disk in a PC), and
put it, or the whole `Content` tree as it is, in one of:

- `Android/data/<the app>/files/dlc/` (for the published apps
  `com.mw2.recomp`, `com.mw3.recomp`, `com.cod4.recomp`, `com.aw.recomp`),
  beside the app's `game` folder; Advanced Warfare's app copies them there
  itself (**Add DLC**);
- a `dlc` folder beside the disc image the game is played from, for instance
  `Download/dlc/` when the image is `Download/mw2.iso`;
- on a PC or in a development run, the folders `MW2_DLC_DIR` names (one, or
  several with semicolons between), or `dlc` beside the game folder.

Folders are searched six levels deep, so the console's tree can be copied
whole. A file that is not a package for this game's title id, or not
downloadable content (type `00000002`), is named in the log and not used. As
many packages are listed as the title asks for: six by the three older
games, every one there by Advanced Warfare (thirteen with the owner's).

Never share a package, and none belongs in an issue or a pull request: it is
content you bought.

## What the runtime does

The three multiplayer executables do the same four things at start-up
(`Content_Init`; in Modern Warfare 2's disc executable `sub_8230A950`,
`sub_8230A6E8`, `sub_8230A328` and `sub_8230A4D8`), and the single-player
ones do none of them (Modern Warfare 3's campaign has the code as well):

1. **List.** `XContentCreateEnumerator(user, device 0, type 2, flags 0, 6
   items)`, then `XEnumerate` into a buffer of six `XCONTENT_DATA` (1848
   bytes) through an overlapped block. `kernel/xam.cpp` hands out the
   packages found (`kernel::ListDownloadedContent` in `kernel/content.cpp`):
   the display name from the package's header and its file name, with content
   type 2. Before this the enumerator was always empty, which is why no title
   ever looked further.
2. **Mount.** For each entry, `XContentCreate(user, "mappack<n>" or
   "dlc<n>", the entry, OPEN_EXISTING, no disposition, &licence, overlapped)`.
   `XamContentCreateEx` mounts the package under that drive name, read-only.
3. **Identify.** The title opens `<drive>:\mappack<i>.id` (Modern Warfare 3:
   `dlc<i>.id`) for `i` from 1 to 6 with its ordinary file calls; the one that
   exists says which pack this is (Modern Warfare 2 keeps it as the flag
   `2 << i`), and a package with none is closed again.
4. **Use.** The flags go into `xblive_mappacks` and `party_mappacks`, the
   menus and the playlists read them, and the fastfiles of the pack's maps are
   opened from the pack's drive.

**No extraction, and no STFS writing.** A package is read where it lies
(`kernel/volume.h`, `ReadVolume::OpenPackage`): its file table is read once,
each file's chain of 4 KB blocks is kept as runs, and a read is a `pread` at
the block's place between the hash tables. Only the read-only layout a
downloaded package has is understood (one hash table a group; `LIVE` and
`PIRS`), as in `tools/stfs.py` and the title update's reader
(`launcher/package.cpp`); a console-signed `CON` package is refused. Hashes
and the signature are not checked: nothing is trusted that the title would
not have read anyway.

**The licence.** No title imports `XContentGetLicenseMask`; the mask comes
back from `XContentCreate`. The runtime reports every licence bit the
package's own header carries (sixteen entries at `0x22C`). The console would
first match each entry against the profile and the console the pack was bought
on; neither exists here, and the package is the player's own. A package with
no licence bits reports none. Modern Warfare 2's disc executable stores the
mask beside the pack and was not seen to test it in the functions above.

## Advanced Warfare: map packs and Exo Zombies

**What a player needs**, all from their own console's hard disk, under
`Content\0000000000000000\41560914`:

| folder | what | needed for |
|---|---|---|
| `00000002` | the map packs you bought: Havoc, Ascendance, Supremacy, Reckoning (each one file of 1.1 to 1.3 GB), the free Atlas Gorge map, and the free **Compatibility Packs** 1 to 6 (0.13 to 0.52 GB each) | the maps; the Compatibility Packs are not optional: they hold what every player of the updated game loads whether or not they bought a pack (the packs' weapons and gear, and in Compatibility Pack 1 the whole of Exo Zombies' common files and its English text), and the game leaves at once without the first |
| `000B0000` | the game's **title update**, one file of 120 MB named `tu00000010_00000000` or the like | everything: the packs are for the updated game |

A console that played the game online has all of these. The two packages
whose names begin `41560914` in `00000002` are the install disc's, which the
app and setup already take from the install disc's image; copying them too
does no harm.

**All six Compatibility Packs.** With one missing the updated game stops at
its multiplayer menu with a notice that required content was added to the
console's downloads and will be there when the download ends, and offers
only the main menu. Nothing downloads here, so the app looks for the packs
itself, each by the marker `dlccompat<n>.id` at the root of its package
(`Dlc.missingCompatibilityPacks`; the name a console shows is another in
another language): **Add DLC** names the ones still to add, its entry on the
start screen counts them (**ADD DLC · 2 MISSING**), the log's `map packs:`
line lists them, and until all six are there the multiplayer is the disc's
and Exo Zombies is not offered.

**Copying them off the console.** Either way the files are only read. The
owner's were read from a copy of his hard disk's `Content` folder; the steps
below are the console's own menus and the tools' own descriptions, and were
not walked through for this port.

- *With the disk in a PC.* Take the hard disk out of the console, connect it
  with a SATA-to-USB adapter, and open it with a tool that reads the
  console's FATX format (FATXplorer, Xplorer360 and others); the folder is
  in the data partition. Windows offers to format the disk when it is
  connected: do not. This gets everything, the title update included.
- *With a USB drive.* On the console: Settings, Storage, the hard drive,
  Games and Apps, Call of Duty: Advanced Warfare; copy the items to a USB
  drive the console formatted. On a PC that drive looks empty: the console
  keeps its files inside hidden container files, which the same kind of
  tool opens (Horizon, USB XTAF Explorer and others). Copy out
  `Content\0000000000000000\41560914`. If the title update is not among
  what the console offered to copy, it has to come from the disk
  (`000B0000` beside the packs' `00000002`; on an old dashboard the
  console's cache partition instead).

**With a PC** (the multiplayer has to be built a second time, as the update
makes it; one command does it and copies the packages to a phone on USB):

    setup\setup.cmd -Game aw -Dlc <the folder you copied: ...\41560914>

    bash setup/setup.sh --game aw --iso <game disc> --install-iso <install disc> --dlc <the folder you copied: .../41560914>

`-Dlc` (`--dlc` on Linux) reads the folder, makes the update's executable
from your disc's and your update's patch, recompiles it, builds it into the
game code beside the two it already has (`com.aw.recomp.game`, 110 MB with
all three), and copies your packages to the phone's
`Android/data/com.aw.recomp/files/dlc` (5.4 GB with all of the owner's).
The app must be v0.1.1 or later.

On Linux the folder is looked at first, so one without the title update
stops in seconds and says what to copy; the two disc images may be given
either way round; and with no phone on USB (`--no-install`, or none
connected) the last lines say what to carry over by hand: `out/aw-game.apk`,
opened on the phone, then the folder, then **Add DLC**. Build the three
libraries with setup and not by hand: the app loads a game library only
when its statement is the one the app was built for
([game-library.md](game-library.md)), and a tree recompiled by hand, with a
step missed or run under another title's settings, states another and is
refused on the phone (a tester's did: `d93b641063f2` where the app v0.1.1
asks for `06fc007e86fc`, which is what setup makes on Windows and on Linux).

**In the app, with no PC at all.** **Add DLC** (shown once the game is
installed), then **Choose a folder** and pick the `41560914` folder (wherever
you put it: the phone's storage, an SD card, a USB drive), or **Choose
files** and pick them in the Files app. The app copies those that are this
game's packages into its `dlc` folder, the title update in `000B0000`
included, and lists them with their names and sizes. Hold an entry to remove
it.

The packs themselves are read as the files they are. What has to be made is
the game code of the multiplayer as the update makes it, and the app makes
it on the phone, by itself, as soon as the update's package has been copied:
in the background, with the notification and the screen of the first build
(**Build the game code**), and with the same toolchain package, which the
app fetches if it is not there
([ondevice-build.md](ondevice-build.md#the-multiplayer-a-title-update-makes)).
The Add DLC screen says how far it is, and the campaign and the disc's
multiplayer can be played meanwhile. When it is done, Multiplayer is the
updated game and **Exo Zombies** is in the launcher. If the build was
stopped or failed, the screen says why and has **Build it now**.

**What changes.** While the title update's package is in the `dlc` folder
and its game code is on the phone, **Multiplayer** starts the updated game
(its version reads 1.20.1388426.0 at the top right): the packs' maps are
under **Bonus Maps** at the top of the map list, in a private match or
System Link. **Exo Zombies** has an entry of its own in the launcher: on
the console it is on the updated campaign's main menu, and the campaign here
stays the disc's. The updated multiplayer and the disc's do not play each
other, as on a console: everybody in a match needs the update, and for a
pack's map the pack. Removing the title update in Add DLC goes back to the
disc's multiplayer. Both use the multiplayer's one profile, and the disc's
game does not take the updated game's: started after it (on Windows, the
update's package taken away), the disc's multiplayer said its custom
classes were reset (`MENU_RESETCUSTOMCLASSES`) and wrote the profile afresh; the
runtime keeps the file it replaced beside it as `.bak`. So going back costs
the classes made since.

On Windows setup writes `Multiplayer with map packs.cmd` and
`Exo Zombies.cmd` beside `Multiplayer.cmd`, and names the folder in `.env`
(`MW2_DLC_DIR`) rather than copying it.

### The owner's packages (read only, never copied into the repository)

`LIVE` packages of title `41560914`, type 2, with one licence entry each;
none is encrypted beyond the fastfiles' own signing, which is checked as the
disc's is and not loosened.

| package | marker | what it holds |
|---|---|---|
| Atlas Gorge Bonus Map (133 MB) | `dlc1.id` | `mp_dam` |
| Havoc DLC (1.2 GB) | `dlc2.id` | Core (`mp_lost`), Urban (`mp_urban`), Sideshow (`mp_clowntown3`), Drift (`mp_torqued`), and Exo Zombies' Outbreak (`mp_zombie_lab`), each with its `_load` and `_path` fastfiles and a patch, an image and a sound pack, and twelve language folders |
| Map Pack 3, Supremacy (1.1 GB) | `dlc4.id` | Parliament (`mp_bigben2`), Kremlin (`mp_kremlin`), Compound (`mp_sector17`), Skyrise (`mp_highrise2`), and Carrier (`mp_zombie_ark`) |
| Map Pack 4, Reckoning (1.3 GB) | `dlc5.id` | Overload (`mp_lair`), Quarantine (`mp_liberty`), Fracture (`mp_fracture`), Swarm (`mp_seoul2`), and Descent (`mp_zombie_h2o`) |
| Compatibility Pack 1 to 6 (0.13 to 0.52 GB) | `dlccompat1.id` to `dlccompat6.id` | `compatibility_pack<n>_mp.ff` and its image and sound packs: what the packs added to every player's game. Pack 1 also has `common_zm_mp`, `common_core_zm_mp`, `ui_zm_mp`, the zombies' movies and, in `english\`, the English of the zombies' and the first update's menus |
| Season Pass (57 KB) | `seasonpass.id` | the marker alone: it is what tells the title its owner has the four packs' entitlement |
| Installer Package 01, 02 (`PIRS`, 2.0 and 1.8 GB) | `dlcinstall1.id`, `dlcinstall2.id` | the install disc's content, as before ([AW-BRINGUP.md](AW-BRINGUP.md)) |
| `tu00000010_00000000`, `tu00000015_00000000` (type `000B0000`, 120 MB each) | | title update 17 for two pressings of the disc (base versions `0x10` and `0x15`): `default.xexp` and `default_mp.xexp`, 52 `patch_*.ff` and the zombies' `ui_zm_mp.ff` and `code_post_gfx_zm_mp.ff`, two image packs, the store's pictures, and one language folder each (`spanishna`, `spanish`) |

**Ascendance (`dlc3.id`) is not among them** under any name: no package has
the marker, or its maps (`mp_perplex`, `mp_spark`, `mp_blackbox`,
`mp_climate_3`, `mp_zombie_brg`); the update's patch files for them are
there and unused. Its maps are not in the menu, as on a console without it.

### Why the title update, and how it is used

The disc's multiplayer (version `0xA`) mounts the packs and knows
`dlc1.id` to `dlc5.id` and `seasonpass.id`, but it has no word of the
Compatibility Packs, of the zombies' files or of a zombies mode: those are
the update's code. The packs' own fastfiles are built for the updated game
too. So the packs are served to the game the update makes:

- **The executable.** `default_mp.xexp` is the console's own delta patch.
  XenonRecomp's patcher applies it to the disc's `default_mp.xex` (it checks
  that the patch was made for this executable by its key; the media id of
  the pressing it does not check, and the owner's two updates, made for
  Latin American and Spanish pressings, give the same image on the English
  disc: SHA-256 `9112629a...af55`, version 0.0.17.x). Setup does it once
  for the recompiler (`aw\tu17`, git-ignored); the game does it in memory at
  every start, from the disc's executable in the game folder and the update
  in the `dlc` folder, and keeps the result nowhere
  (`runtime/install/install.cpp`). A delta patch moves parts of the image
  over themselves, and the patcher does that with `memcpy`, which need not
  copy overlapping bytes in order: on the AYN Thor the update came out as
  another executable (27,793 bytes of it) and the game refused it. The
  runtime compiles the patcher with its copies as moves
  (`runtime/install/xex_patcher_moves.cpp`). The campaign's patch (`default.xexp`) is
  refused by the patcher for this disc's `default.xex` and was not followed
  up: the campaign stays the disc's.
- **A build of its own**, `awmp17` (`config/AWMP.tu17.toml`, the block
  `AW_TITLE_TU17` of `runtime/title.h` with every address derived again,
  `config/abi/awmp17`): 22,969 functions, 137 jump tables, the same 415
  stepped instructions. Two things the disc's executables did not need: the
  record forms of two rotate instructions the recompiler writes without
  their condition (`PpcRecordCr0`; the Lua interpreter's test of a boolean
  went wrong without it), an unpack of half floats in place, and a jump
  table of halfwords with no bound before it, which the analyser passes
  over (`tools/intext_switch_tables.py`).
- **The update's files** are mounted as `update:`, where the console has
  them and the title looks first (`kernel::MountTitleUpdate`).
- **An update of another language than the disc** (the owner's are Latin
  American Spanish and Spanish; the disc is English) has none of English's
  patch fastfiles, and this game keeps its tables in them: without
  `mp/ingamestore/s1_mdlcEntitlements.csv` no pack is identified and the
  title leaves with `DLC_DLCREMOVED`. So the update's own localised
  fastfile is loaded, under its own name, before the English one the title
  asks for; its strings are left out for English's (the title has room for
  one language's), and a string English has not is answered with the
  update's, in its language (`runtime/aw_update_language.cpp`). English the
  update would have brought is taken from the player's packs where they
  have it (Compatibility Pack 1). With the owner's files 854 of some 13,000
  strings are left in Spanish: mostly the later packs' map descriptions,
  gear and challenges. The maps' and modes' names are English. An update
  made for an English disc needs none of this and gets none.

### What was verified

In [AW-BRINGUP.md](AW-BRINGUP.md#map-packs-and-exo-zombies-25-november-2026).

## Playlists, Find Match and other players

- **Private matches and System Link** are where a pack's maps can be played:
  the host chooses the map, and the title's own check (`partyismissingmappack`,
  `party_membersMissingMapPack`) keeps a party from starting a map a member
  does not have. Every player needs their own package.
- **Find Match through the relay** is unchanged for players without packs. The
  playlists the project serves (`relay/playlists*.info`) name only the discs'
  maps and have no `requiredDlcPack` line, so no public playlist rotates a
  pack's maps.
- **A caution.** The title puts its map pack flags in every search
  (`XSessionSearchEx`: "mapPack flags of %i"), and the stand-in for Live's
  matchmaking compares the properties a search carries for equality
  (`kernel/xam.cpp`, [multiplayer.md](multiplayer.md)). A player with a pack
  mounted may therefore find only players with the same packs in Find Match.
  This follows from the code and was not measured; taking the package out of
  the `dlc` folder undoes it.

## What is tested

- `tests/kernel_files_test.cpp` builds a read-only package byte by byte (a
  marker, a folder, a file that runs past the first hash table, two files
  with interleaved blocks) and reads it through the title's own file calls:
  mounted as `mappack1:`, case-insensitive, at positions and in sequence,
  read-only, unmounted; a truncated package and a file that is not one are
  refused.
- `tests/kernel_xam_test.cpp`: a type 2 enumerator lists the packages, as
  many as the buffer holds each time, and ends with no more files.
- Two larger synthetic packages, made on the test PC from the owner's own
  Modern Warfare 2 disc and never kept in the repository (a `mappack1.id` and
  four of the disc's fastfiles, 121 MB, so past the second level of hash
  tables). Both read back identical through `tools/stfs.py`. In Modern
  Warfare 2's multiplayer on a Retroid Pocket Flip 2, from
  `Android/data/<app>/files/dlc/`:
  - **Rust played out of the package.** With `mp_rust.ff` and
    `mp_rust_load.ff` taken out of the game folder and put in the package
    under their own names, the title listed the package, mounted it as
    `mappack1:`, read `mappack1.id`, and at `map mp_rust` opened both
    fastfiles from `mappack1:` and went into the match (56 frames a second in
    play). It also asked for `mappack1:\mappack1.arena`, the pack's list of
    its maps, which the synthetic package has not got and a real one has.
  - **A renamed fastfile is refused by the title itself.** With the disc's
    Rust under the name `mp_complex.ff`, `map mp_complex` looked on the disc,
    then opened `mappack1:\mp_complex_load.ff` from the package, and stopped
    at the title's own signature check ("dirty disc"): the disc's fastfiles
    are signed under their names (`runtime/unsigned_fastfiles.cpp`). So a package's files
    go through the same check as the disc's; nothing here loosens it.

## What is untested

- **A real map pack of Modern Warfare 2, Modern Warfare 3 or Call of Duty
  4.** The owner has none.
- **Modern Warfare 2 and 3 with their retail packs on the disc's game
  version.** Both games' packs came out after title updates, and the port
  plays the disc's executables (Modern Warfare 2 1.0.557). The disc
  executables have the whole mechanism above, but the packs' fastfiles and
  their menu zones (`dlc1_ui_mp.ff` and `dlc2_ui_mp.ff` came with title
  updates, not with the packs) were built for the updated game: maps may be
  missing from the menus, or refuse to load.
- **Call of Duty 4's Variety Map Pack**: the same mechanism, read from the
  code alone.
- **The licence bits of a real package** of those three games, and whether
  any of them refuses a mask the runtime reports. Advanced Warfare takes
  the mask of the owner's packages as it is.
- **Advanced Warfare:** Ascendance (the owner has no package of it), an
  update made for an English disc, the campaign with the update, and the
  updated multiplayer's code built on the device.
- **Find Match with a pack mounted** (the caution above).
- Packages larger than the synthetic one, and any with folders inside, beyond
  the unit test's.
