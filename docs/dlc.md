# Downloadable content (map packs)

**The short answer for a player.** The games on the discs are complete and
this project ships no map packs, and never will: they are sold content. If
you bought a map pack on your own Xbox 360, the port can read **your own
package files from your console's hard disk**: copy them off the console
([how](#copying-your-packs-off-an-xbox-360-hard-drive)), then in the app
**Add DLC → Choose a folder**. The app copies them beside the game and the
game mounts them as the console did. Nothing is unlocked for anybody: a
player without the pack's file has not got the map.

| game | folder on the console's hard disk | state |
|---|---|---|
| Modern Warfare 3 | `Content\0000000000000000\415608CB\00000002` | **works with real packs**: all 20 multiplayer maps, 13 Survival maps and 8 Special Ops missions of the owner's 26 packages ([Modern Warfare 3](#modern-warfare-3-map-packs-survival-maps-and-special-ops-missions)) |
| Advanced Warfare | `Content\0000000000000000\41560914` (`00000002` and the title update in `000B0000`) | **works with real packs**, and needs the title update ([Advanced Warfare](#advanced-warfare-map-packs-and-exo-zombies)) |
| Modern Warfare 2 | `Content\0000000000000000\41560817\00000002` | **untested with a real pack**: a test package made from the disc's own files is listed, mounted and played from ([what is tested](#what-is-tested)) |
| Call of Duty 4 | `Content\0000000000000000\415607E6\00000002` | **untested with a real pack**: the same, with a test package |

**Online, everybody needs the map.** A pack's map is played in a private
match, System Link or Local Play, and each player in it needs the pack's
file on their own device: the game loads the map from it. A player who
joins without it is told the map's file could not be found and is back at
the menu.

## What a player supplies

A map pack on the console is one file: an STFS package (it begins with `LIVE`,
or `PIRS`), with no extension and a name of forty characters, kept on the hard
disk under

    Content\0000000000000000\<title id>\00000002\<package>

| game | title id | drive the title mounts | marker inside |
|---|---|---|---|
| Modern Warfare 2 | `41560817` | `mappack1:` ... | `mappack1.id` to `mappack6.id` |
| Modern Warfare 3 | `415608CB` | `dlc1:` to `dlc5:` | `dlc1.id` to `dlc26.id`, one in each package |
| Call of Duty 4 | `415607E6` | `mappack1:` ..., closed again at once | none |
| Advanced Warfare | `41560914` | `dlc1:` ... | `dlc1.id` to `dlc5.id`, `dlccompat1.id` to `dlccompat6.id`, `seasonpass.id` |

Copy the package off your own console's disk
([below](#copying-your-packs-off-an-xbox-360-hard-drive)), and give it to
the game in one of these ways:

- **In the app: Add DLC** (in the launcher of all four games, once the game
  files are installed), then **Choose a folder** and pick the folder you
  copied (the game's `415608CB` or the like, its `00000002`, or any folder
  above them: wherever it is, on the phone, an SD card or a USB drive), or
  **Choose files** and pick the packages in the Files app. The app copies
  those that are this game's into `Android/data/<the app>/files/dlc/`,
  beside its `game` folder, and lists them with their names and sizes; hold
  an entry to remove it. Nothing is built and no PC is needed, except for
  Advanced Warfare's title update, which the app builds on the phone by
  itself.
- **With setup, from a PC with the phone on USB:**
  `setup\setup.cmd -Game mw3 -Dlc <the folder>` on Windows (`-Game mw2`,
  `cod4`, `aw`), `bash setup/setup.sh --game mw3 --dlc <the folder>` on
  Linux. It reads the folder, copies the packages to the app's `dlc` folder
  and, for the Windows game, names the folder in `.env` (`MW2_DLC_DIR`).
- **By hand:** into `Android/data/<the app>/files/dlc/` (the published apps
  are `com.mw2.recomp`, `com.mw3.recomp`, `com.cod4.recomp`,
  `com.aw.recomp`); or a `dlc` folder beside the disc image the game is
  played from, for instance `Download/dlc/` when the image is
  `Download/mw2.iso`; on a PC, the folders `MW2_DLC_DIR` names (one, or
  several with semicolons between), or `dlc` beside the game folder.

Folders are searched six levels deep, so the console's tree can be copied
whole. A file that is not a package for this game's title id, or not
downloadable content (type `00000002`), is named in the log and not used. As
many packages are listed as the title asks for: six by Modern Warfare 2 and
Call of Duty 4, every one there by Advanced Warfare (thirteen with the
owner's); Modern Warfare 3's table holds five, and the runtime joins the
rest onto the first one's drive (26 with the owner's).

Never share a package, and none belongs in an issue or a pull request: it is
content you bought.

## Copying your packs off an Xbox 360 hard drive

Either way the files are only read, and they stay yours. The owner's
packages were read from copies of his own console's `Content` folder; the
steps below are the console's own menus and the tools' own descriptions,
and were not walked through for this port.

- **With the disk in a PC.** Take the hard disk out of the console (it
  slides out of an Xbox 360 S or E; on an older console it is the grey
  block on the side), connect it with a SATA-to-USB adapter, and open it
  with a tool that reads the console's FATX format (FATXplorer, Xplorer360
  and others). The games' content is in the data partition, under
  `Content\0000000000000000\<the game's number>`. Windows offers to format
  the disk when it is connected: do not. This gets everything, Advanced
  Warfare's title update included.
- **With a USB drive.** On the console: Settings, Storage, the hard drive,
  Games and Apps, the game; copy its items (each map pack is one) to a USB
  drive the console formatted. On a PC that drive looks empty: the console
  keeps its files inside hidden container files, which the same kind of
  tool opens (Horizon, USB XTAF Explorer and others). Copy out
  `Content\0000000000000000\<the game's number>`.

What to copy, for each game (the whole folder is fine, and so is anything
above it):

| game | folder | what is in it |
|---|---|---|
| Modern Warfare 3 | `Content\0000000000000000\415608CB\00000002` | one file for each Collection, Elite drop or free map: 7 MB to 190 MB each, 2.6 GB for all 26 |
| Modern Warfare 2 | `Content\0000000000000000\41560817\00000002` | the Stimulus and Resurgence packages |
| Call of Duty 4 | `Content\0000000000000000\415607E6\00000002` | the Variety Map Pack |
| Advanced Warfare | `Content\0000000000000000\41560914` | `00000002` (the packs and the Compatibility Packs) **and** `000B0000` (the title update), 5.4 GB with all of the owner's |

The files have no extension and names of letters and digits. A package
bought on another account or console is the same file; the port does not
ask whose it is, and it is for you to copy only what you bought.

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

Call of Duty 4's disc executable stops after the second: it mounts the
package, closes it again at once and asks for every fastfile on the disc,
so the runtime looks in the player's packages for a file the disc has not
(`kernel/files.cpp`, [what is tested](#what-is-tested)). Modern Warfare 3's
packs past the five its table holds are handled
[below](#what-the-discs-executables-lacked-and-what-the-runtime-does).

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

## Modern Warfare 3: map packs, Survival maps and Special Ops missions

**What a player needs:** the packages in
`Content\0000000000000000\415608CB\00000002` on their console's hard disk,
and nothing else. The disc's two executables play them: no title update is
needed and nothing is built.

**In the app.** **Add DLC**, **Choose a folder**, pick the folder. Then:

- **Multiplayer:** the packs' maps are under **BONUS MAPS**, the first row
  of the map list, in a private match, Local Play or System Link, with bots
  or other players. Everybody in the match needs the map's pack.
- **Special Ops:** the launcher has **SPEC OPS MAP PACKS**, which lists the
  Survival maps and missions your packages hold by name; choose one and the
  game starts in it, for one player. The disc's own Special Ops menu lists
  the disc's sixteen missions and sixteen Survival maps, written out one by
  one, and has no rows for the packs': those came with title updates.
  **Return to Special Ops** afterwards goes to the game's own menu.

On Windows, `setup\setup.cmd -Dlc <the folder>` names the folder in `.env`;
a level of the packs is started with `MW2_CONTENT_LEVEL=<level>` there
(`so_survival_mp_italy`, `so_escape_hamburg`: the name of its `so_*.ff`).

### The owner's Modern Warfare 3 packages (read only, never copied into the repository)

26 `LIVE` packages of title `415608CB`, type 2, 2.6 GB together:

| what | packages | levels |
|---|---|---|
| Multiplayer maps with a Survival map each | Piazza, Liberation, Overwatch, Black Box, Sanctuary, Foundation, Oasis, Terminal (free), Decommission, Offshore, Gulch, Boardwalk, Parish | `mp_italy`, `mp_park`, `mp_overwatch`, `mp_morningwood`, `mp_meteora`, `mp_cement`, `mp_qadeem`, `mp_terminal_cls`, `mp_shipbreaker`, `mp_roughneck`, `mp_moab`, `mp_boardwalk`, `mp_nola`, and `so_survival_<map>` for each |
| Face Off maps | Getaway and Lookout, Erosion and Aground (free), U-Turn and Vortex, Intersection | `mp_hillside_ss`, `mp_restrepo_ss`, `mp_courtyard_ss`, `mp_aground_ss`, `mp_burn_ss`, `mp_six_ss`, `mp_crosswalk_ss` |
| Special Ops missions | Black Ice, Negotiator, Kill Switch, Iron Clad, Arctic Recon, Vertigo, Light 'Em Up, Special Delivery | `so_escape_rescue_2_so`, `so_infiltrate_intro`, `so_coop_vlad_ep01`, `so_escape_hamburg`, `so_demolition_slava_hunt`, `so_rooftop_dubai_specops`, `so_spotlight_castle`, `so_support_dam_specops` |
| Survival Chaos | one package of 57 KB | the marker alone: Chaos mode is code of a title update, which the disc's game has not got |

Each has its marker (`dlc1.id` to `dlc26.id`), the levels' fastfiles and
their loading pictures, and the files its textures and sounds stream from
(`imagefile6.pak` to `imagefile30.pak`, `packfile2.pak` to
`packfile101.pak`). A mission's package also has its text in nine languages
(`english_so_escape_hamburg.ff` and the like).

### What the disc's executables lacked, and what the runtime does

The packs' fastfiles are of the disc's version and the disc's executables
load them. Everything else the packs needed came with title updates, which
are code and not in a player's packages:

- **More than five packs.** The title tells a pack by its marker, knows
  `dlc1.id` to `dlc6.id`, and keeps the packs in a table of five; a sixth's
  name is written past its end. The packages past what the table holds are
  joined onto the first one's drive, where the title looks for a map's
  fastfile (`kernel/content.cpp`).
- **The maps' names.** The menus are ready for packs (the BONUS MAPS row),
  but the disc's arena file names the disc's sixteen maps. For each
  `mp_*.ff` in the player's packages an arena is added: the map, the name
  the game gave it, the pack it is in. The list's cursor and height, wrong
  on the disc where no map could show it, are put right
  (`mw3_dlc_maps.cpp`).
- **Animated props.** Ten of the twenty maps stopped with "animation ... not
  defined in anim tree": the update's animation trees name them. The names
  a level's scripts use are added to the tree as it is parsed
  (`mw3_pack_anims.cpp`).
- **Teams.** A map's fastfile holds the soldiers of its own two teams; the
  update's arena said which. They are found by what the fastfile holds, and
  the map's arena is given them (`mw3_dlc_maps.cpp`).
- **Streamed files.** The disc's tables of open files hold 21 image files
  and 10 sound files by number, unchecked; a later pack's `imagefile21` or
  `packfile10` wrote past them ("Read error of file"). A pack's file is
  given a free place in the table the executable has (`mw3_pack_files.cpp`).
- **Special Ops.** The campaign's executable loads a pack's Survival map
  or mission, with the same two fixes above, but its menus cannot name one:
  the launcher does, and the level is loaded with the console's `map` once
  the title has mounted the packages (`MW2_CONTENT_LEVEL`,
  `kernel/content.cpp`).
- **A mission's text.** The disc is of one language and its levels hold
  their own text; a pack's mission has it in a fastfile beside the level's
  for each language, which the updates load. The English one is loaded with
  the level (`mw3_pack_text.cpp`); without it the objectives and hints were
  their keys.

Nothing is listed that the player's packages do not hold, and a map's own
fastfile is what loads, checked as the disc's are.

### Known gaps

- The lobby's preview of a pack's map is a placeholder (a checker): the
  previews came with the title updates.
- A Special Ops level of the packs is started from the launcher, for one
  player: the disc's lobby cannot be told a level it has no row for, so
  there is no two-player start.
- Decommission's Survival map logs an unknown animation tree (`chicken`)
  and plays.
- Chaos mode and the Face Off playlists are the updates' code and are not
  there. The Face Off maps are played as ordinary maps.
- Find Match: as [below](#playlists-find-match-and-other-players).

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

### What each executable is given, and what is checked first

The game is three executables here (the disc's campaign, the disc's
multiplayer, the multiplayer as the update makes it), and a package goes
only to those that can use it. A tester with seventeen packages and two
title updates in the `dlc` folder chose Campaign and got the game's own
"An installed Content Pack is corrupt. It must be deleted to continue."
with nothing but Exit; taking the Compatibility Packs out again was the
only cure. Nothing of his was corrupt, and two things had to meet:

- The disc's executables list the console's downloads, mount each package
  and look for the marker of a pack in their own table (`dlcinstall1`,
  `dlcinstall2`, `dlc1` to `dlc5`, `seasonpass`, `mdlc0` to `mdlc5`: the
  table at `8281FC08` of the campaign, `8288D2A0` of the multiplayer). A
  package with none of them, which every Compatibility Pack is, is closed
  again and noted as a corrupt one (`8249AD90`: the marker search `8249A250`
  answers 1, and the byte at `843A3B61` is set). That alone shows nothing:
  the menu asked for the byte (`AnyContentDamaged`, `821A41B0`) in no run
  that had both of the install disc's packages on the list, and on choosing
  Campaign in the run that had one of them left off.
- Their list holds twelve packages (`843A7D28`, 308 bytes each), and the
  app found the player's packs in the `dlc` folder before the install
  disc's two in its image. Eleven packs or more therefore left the second
  install package, or both, off the list.

Reproduced on the AYN Thor with the app as released (v0.1.2) and the
owner's eleven packs listed ahead of the install disc's: the notice, on
choosing Campaign. With ten packs, or with the install disc's two listed
first, there is none; fifteen and nineteen packages listed after the two
showed none on Windows either. The list is also read to the first empty
entry, so the entry after the twelfth is read out of what lies behind the
list (a table of file-name hashes, empty until a pack has been mounted). A
console never gets there: it starts the updated executables once the
update is on it.

| a package with | is | the disc's campaign and multiplayer | the updated multiplayer |
|---|---|---|---|
| `dlcinstall1.id`, `dlcinstall2.id` | the install disc's two | given | given |
| `mdlc0.id` to `mdlc5.id` | a bonus item (an entitlement, no files of a game version) | given | given |
| `dlc1.id` to `dlc5.id` | a map pack, built for the updated game | not given: the disc's multiplayer would list its maps and fail to load them | given |
| `seasonpass.id` | the entitlement to the map packs | not given | given |
| `dlccompat1.id` to `dlccompat6.id` | a Compatibility Pack | not given: the disc's executables do not know the marker | given |
| no such marker | nobody's | not given | not given |

The install disc's packages are listed first, for every executable, in
whatever folder or image they were found. Of two packages that are the
same pack (a folder put together from two
consoles, or an older and a newer version) the one with the higher version
in its header is given and the other is not. Of several title updates each
is tried until one makes this build's executable: an update is made for one
pressing of the disc, and one for another pressing or of another version
does not fit. The rules are `runtime/kernel/content_route.h`; the log has a
line for every package: what its header says (title, content type, version,
licence bits), which pack it is, and whether this executable was given it.

**Before any of that, each package is checked against what it says of
itself**, so that a copy gone wrong is said in a sentence that names the
file and not by the game:

| what is wrong | how it is told | where | what is said |
|---|---|---|---|
| not a package at all, or a console-signed one (a save, a profile) | no `LIVE` or `PIRS`, or two hash tables a group | everywhere | skipped; in the app's list "… is not a package from a console's hard disk." |
| another game's | the title id in the header | Add DLC, setup, the game's start | "… is for another game (415608CB), not this one." |
| another kind of content (an avatar item, a theme) | the content type | Add DLC, the game's start | "… is not a map pack or title update of this game." |
| a damaged header | the package's SHA-1 of its header (`0x32C`, over everything from the content type on) | Add DLC, setup, the game's start | "… is damaged: copy it again from your console." |
| a damaged block | the SHA-1 of every 4 KB block, kept in the hash table before each group of 170 | Add DLC as it copies, setup as it lists (18 s for the owner's 10 GB) | the same; the copy is not kept |
| a copy that stopped part-way | the block count in the header (`0x395`) against the file's length | Add DLC, setup, the game's start | "… is incomplete: copy it again from your console." |
| the same pack twice | two packages with one marker | Add DLC's list, the game's start | "… is here twice; the newer copy is used." |
| no title update | no package of type `000B0000` | Add DLC, setup | the map packs are listed and the multiplayer stays the disc's; Add DLC and setup say which folder to copy |
| a title update that does not fit the disc | the executable it makes, by its SHA-256 | the game's start, the build on the phone | the next update is tried; with none that fits, "No title update in the dlc folder fits this disc …" |
| a Compatibility Pack missing | the markers present | Add DLC, the launcher | "Still missing: Compatibility Pack 3" (as before) |

The blocks are hashed where the file is read whole anyway and not at every
start (ten gigabytes). The signature over the header is the console
maker's and is not checked: the app has no key of theirs. A package whose
header was edited without its hash being made again (a licence patched by
hand) is therefore refused as damaged, though a modified console would
take it; the project is for packages as the console wrote them.

Tested with packages made by the tests, never a game's:
`tests/content_route_test.cpp` (the table above, duplicates, versions),
`tests/kernel_files_test.cpp` (a damaged header, two kinds of cut-short
file, the version), `PackageCheckTest.kt` (every row of the second table
that Add DLC tells).

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
  (`runtime/install/xex_patcher_moves.cpp`). The campaign's patch
  (`default.xexp`) is for this disc's `default.xex` (its key matches) and the
  patcher still refuses it (`XexFileInvalid`): it sizes its buffer for the
  updated image, and the disc campaign's image is the larger one. That was
  not followed up, so the campaign stays the disc's, and with it the main
  menu: the update's campaign is where a console has its Exo Zombies entry
  (the updated multiplayer's own menus have none, in Multiplayer or in Exo
  Survival, and the update's patch fastfiles loaded into the disc's
  campaign leave its menus black).
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
- **The same two games again on Windows, 26 November 2026**, with the
  packages made anew from each disc's own files:
  - *Modern Warfare 2* (`mappack1.id`, `mp_rust.ff`, `mp_rust_load.ff`,
    33 MB): listed, mounted as `mappack1:`, `mappack1.arena` asked for, and
    `map mp_rust` played at 58 frames a second.
  - *Call of Duty 4* (`mp_shipment.ff` and `mp_shipment_load.ff`, 52 MB,
    both taken out of the game folder): the disc's executable lists the
    package, mounts it as `mappack1:` and closes it again in the same
    millisecond. It only notes that the player has a pack: it asks for
    every fastfile as `D:\<name>.ff`, the disc, and the pack's drive in
    that name came with the title update the Variety Map Pack was sold
    after. So `map mp_shipment` ended in "Could not find zone". The runtime
    now looks in the player's packages for a file the disc has not
    (`kernel/files.cpp`), and Shipment loaded from the package and played
    at 60 frames a second. The disc's menus have no rows for the Variety
    Map Pack's four maps; with a real pack they would be started from the
    console (`map mp_killhouse`).
- **Modern Warfare 3 with the owner's 26 packages on an AYN Thor,
  26 November 2026**, in two rounds.
  - *The app's own way in* (a test app that, as found afterwards, ran on
    Qualcomm's driver: its picture does not count, what the app did does):
    **Add DLC → Choose a folder** copied the packages (2.6 GB) in 6 s and
    nothing was built; the launcher listed them under **SPEC OPS MAP
    PACKS**, and Iron Clad chosen there started the mission, its text in
    English.
  - *The picture and the frame rate*, on the bundled Turnip driver (the
    log's driver line read before the picture was judged), the packages
    pushed into `files/dlc` over USB, the maps started from the console
    (`set ui_mapname` in a private match's lobby) and the Special Ops levels
    by `MW2_CONTENT_LEVEL`, each run recorded, a frame every three seconds
    looked at and the recording put through `tools/visualqa`
    (`analyse --mask touch`):

    | what | frames a second | picture | visualqa |
    |---|---|---|---|
    | multiplayer, Piazza (`mp_italy`), six bots | 118.9 | clean | pass |
    | multiplayer, Parish (`mp_nola`), six bots | 117.1 | clean in the sampled frames | **fail**, `local-flicker` three times |
    | Survival on Parish (`so_survival_mp_nola`) | 118.7 | clean; a player who stands still dies after 25 s, on Windows too | pass |
    | Special Ops mission Iron Clad (`so_escape_hamburg`) | 118.3 | clean | pass, one remark |

    Parish's three are all between the player's death and respawn: the
    blur of the death, the killcam, and the respawn. The first two are
    what the tool's own scenarios leave out as a fight. The third is a
    fault, and not a cut of the killcam: for 0.4 s after the respawn, in
    the player's own view, the buildings on the right are missing in some
    frames (the sky shows) and there in the next, a seventh of the picture.
    A killcam and respawn recorded the same way on a disc map (Seatown,
    the same build and phone) show nothing like it, in one run of each. It
    is open, with the single frames of missing geometry seen on disc maps
    of the other games.

    The first round showed a grid of 32-pixel blocks and missing ground at
    54 to 82 frames a second on these maps. That was the test app and not
    the packs: built without the bundled driver, it showed the same on a
    disc map ([android.md](android.md#6-custom-vulkan-drivers-turnip--libadrenotools)).
  - *Between two phones through the public relay*, 26 November 2026
    (`mw3.yassin.app`, the phones on different networks, both on the
    bundled Turnip, the packages in `files/dlc` on both): the Retroid
    Pocket Flip 2 hosted a System Link game on Piazza (`set ui_mapname
    mp_italy` in the lobby), the Thor's list showed it as "Piazza" and
    joined. Both walked the map for three minutes at 119.6 and 60.6 frames
    a second with the same two players on both scoreboards and the clocks
    a second apart, and neither log has a `Com_Error` or a time-out. Before
    the Thor had the packages its list showed the same game as `mp_italy`.
    The lobby's picture of the map is a checked placeholder. The Thor's
    recording passes `tools/visualqa`; the Flip's fails `blocks` on the
    bars of a gate behind the scoreboard, which is the map's own grid.
- **Add DLC pressed in the Modern Warfare 2 and Call of Duty 4 apps, on a
  Retroid Pocket Flip 2, 26 November 2026**, each with a package made from
  the phone's own disc image (Rust with a `mappack1.id`, 35 MB; Shipment,
  55 MB) in a folder beside Download: **Add DLC → Choose a folder** copied
  it into `files/dlc`, the screen listed it, and the multiplayer's log
  mounted it as `mappack1:` (Call of Duty 4 closing it again at once, as
  on Windows). The first press found a fault: both apps said six
  Compatibility Packs of Advanced Warfare were missing; only Advanced
  Warfare's app looks for them now (`Dlc.kt`).

## What is untested

- **A real map pack of Modern Warfare 2 or Call of Duty 4.** The owner has
  none.
- **A map of a pack played from the package on a phone** in Modern Warfare
  2 or Call of Duty 4: the phones play from the disc image, which has the
  test packages' maps itself, so the title had no need of the package
  there. On Windows it was played from one (above).
- **Modern Warfare 2 with its retail packs on the disc's game version.**
  They came out after title updates, and the port plays the disc's
  executable (1.0.557). It has the whole mechanism above, but the packs'
  fastfiles and their menu zones (`dlc1_ui_mp.ff` and `dlc2_ui_mp.ff` came
  with title updates, not with the packs) were built for the updated game:
  maps may be missing from the menus, or refuse to load. Modern Warfare 3's
  packs needed five such repairs
  ([above](#what-the-discs-executables-lacked-and-what-the-runtime-does)).
- **Call of Duty 4's Variety Map Pack**: whether the disc's executable
  loads its fastfiles, which were built for the updated game.
- **Modern Warfare 3:** the Collections as single packages (the owner's are
  the Elite drops, one level or two each), a two-player Special Ops level
  of the packs, and a language other than English.
- **The licence bits of a real package** of those three games, and whether
  any of them refuses a mask the runtime reports. Advanced Warfare takes
  the mask of the owner's packages as it is.
- **Advanced Warfare:** Ascendance (the owner has no package of it), an
  update made for an English disc, the campaign with the update, and the
  updated multiplayer's code built on the device.
- **Find Match with a pack mounted** (the caution above).
- Packages larger than the synthetic one, and any with folders inside, beyond
  the unit test's.
