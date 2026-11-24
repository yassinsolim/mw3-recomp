# Downloadable content (map packs)

**The short answer for a player.** The games on the discs are complete and
this project ships no map packs, and never will: they are sold content. If
you bought a map pack on your own Xbox 360, the port can read **your own
package file from your console's hard disk**: put it in a `dlc` folder and
the multiplayer mounts it as the console did. That path is written and is
tested with a synthetic package made from a disc's own files; **it has not
been tried with a real map pack**, because the project owns none, and for
Modern Warfare 2 and 3 a real pack was made for a later game version than the
disc's, which the port plays. Treat it as experimental until somebody who
owns one reports ([what is untested](#what-is-untested)).

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

Copy the package off your own console's disk (a USB stick formatted by the
console and a tool that reads its `Content` folder, or the disk in a PC), and
put it, or the whole `Content` tree as it is, in one of:

- `Android/data/<the app>/files/dlc/` (for the published apps
  `com.mw2.recomp`, `com.mw3.recomp`, `com.cod4.recomp`), beside the app's
  `game` folder;
- a `dlc` folder beside the disc image the game is played from, for instance
  `Download/dlc/` when the image is `Download/mw2.iso`;
- on a PC or in a development run, the folder `MW2_DLC_DIR` names, or `dlc`
  beside the game folder.

Folders are searched six levels deep, so the console's tree can be copied
whole. A file that is not a package for this game's title id, or not
downloadable content (type `00000002`), is named in the log and not used. Up
to six packages are listed, which is what each title asks for.

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

- **A real map pack, of any of the three games.** The owner has none.
- **Modern Warfare 2 and 3 with their retail packs on the disc's game
  version.** Both games' packs came out after title updates, and the port
  plays the disc's executables (Modern Warfare 2 1.0.557). The disc
  executables have the whole mechanism above, but the packs' fastfiles and
  their menu zones (`dlc1_ui_mp.ff` and `dlc2_ui_mp.ff` came with title
  updates, not with the packs) were built for the updated game: maps may be
  missing from the menus, or refuse to load.
- **Call of Duty 4's Variety Map Pack**: the same mechanism, read from the
  code alone.
- **The licence bits of a real package**, and whether any title refuses a
  mask the runtime reports.
- **Find Match with a pack mounted** (the caution above).
- Packages larger than the synthetic one, and any with folders inside, beyond
  the unit test's.
