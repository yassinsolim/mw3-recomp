# Security fixes

The game's network code is the 2009 original. The runtime closes the known
holes in it: published CVEs, the bugs iw4x fixed in the PC version of the same
engine, and the ones found in this port. Each fix sits in its own file. A fix
in `runtime/` hooks the title function that misreads its input, and its header
comment describes the defect and the rule it applies.

Where a message breaks the game's own limits, the fix refuses it the way the
game refuses a malformed message (the message is marked overflowed and the
game drops it), or the way the sender's own code would never have written it.
Valid traffic passes untouched.

The sender columns:

- **anyone**: any machine that can reach the game. On the `lan` service that is
  the whole local network; on `steam`, the members of your lobby and those who
  were, while their link lasts; any Steam player while you host a System Link
  game, and from opening Find Game until about 40 seconds after you leave its
  game; any game listed in System Link once you have looked for games there (on
  the search's ports, for the rest of the run); and a System Link host you chose
  to join, for the rest of the run. On `server`, everyone on the relay:
  that is who Find Game puts you in a match with.
- **client**: a player connected to a game you host.
- **host**: the host of a game or party you joined.
- **partner**: the other player in a campaign co-op game.

## Multiplayer

| fixed | sender | file |
|---|---|---|
| CVE-2019-20893: a party join request's subparty count overflows a stack array | anyone | `runtime/joinparty_overflow.cpp` |
| CVE-2018-10718, and the same bug on the client: Huffman decompression of a game packet writes past its buffer | client, host | `runtime/huffman_overflow.cpp` |
| The relay packet forwards any packet to another party member without checking who sent it | anyone | `runtime/relay_reflection.cpp` |
| A forged reliable-command acknowledge freezes the host | client | `runtime/sv_reliable_freeze.cpp` |
| A party session's slot count from the network bounds a walk over the 18 member records | anyone | `runtime/party_slot_count.cpp` |
| The client takes game packets, connect replies and host-migration messages from any address, not only its server | anyone | `runtime/client_packet_sender.cpp` |
| The client runs the `activeAction` dvar, which the host can set, as console commands | host | `runtime/active_action_exec.cpp` |
| The host can set dvars the client later runs as console commands | host | `runtime/command_dvars_from_host.cpp` |
| A map name from the network is pasted into a `map` command, so text after a command separator runs as more commands | host | `runtime/map_name_command.cpp` |
| A player's name is used as a print format on the server | client | `runtime/sv_command_time_format.cpp` |
| A server's disconnect reason is used as a print format on the client | host | `runtime/disconnect_reason_format.cpp` |
| A playlist map index and the party's play history index the playlist table without a bound | host | `runtime/playlist_map_index.cpp` |
| A snapshot's hud element field index writes outside the player state | host | `runtime/hud_elem_field_index.cpp` |
| A baseline snapshot with more than 1024 entities writes past the baseline array | host | `runtime/snapshot_entity_count.cpp` |
| A snapshot's entity, player-state and objective field indexes write outside their structures | host | `runtime/snapshot_delta_field_index.cpp` |
| The game state streamed during host migration is read outside its buffer | host | `runtime/migration_state_size.cpp` |
| Restoring a migrated game state takes script object numbers, script code positions and entity pairs without a bound | host | `runtime/script_save_ids.cpp` |
| UI directive expansion overflows a stack buffer | host | `runtime/ui_directive_overflow.cpp` |

## Campaign and Special Ops co-op

| fixed | sender | file |
|---|---|---|
| The co-op launch, accept and lobby heartbeat messages index the two player slots and the lobby's member records without a bound, and a launch or heartbeat without its terminator never ends | partner | `runtime/coop_slot_index.cpp` |
| The co-op launch's map name is pasted into a `map` command | partner | `runtime/map_name_command.cpp` |
| A co-op frame's commands run for a null client when no client was selected first | partner | `runtime/coop_frame_client.cpp` |
| A co-op frame's length is not compared with the packet, and the copy reads past its end | partner | `runtime/coop_frame_size.cpp` |
| The glass state's piece count is not compared with its 1024-piece table (the message is built locally; hardening) | none | `runtime/glass_piece_count.cpp` |
| UI directive expansion overflows a stack buffer | partner | `runtime/ui_directive_overflow.cpp` |

## Modern Warfare 3 multiplayer

MW3's multiplayer (`iw5mp_ship.exe`, [MW3-MP-BRINGUP.md](MW3-MP-BRINGUP.md)) is
the same engine one game on. Each fix above was looked for in that executable
by what its function does (the out-of-band command tables, the message readers
it calls, the globals it reads), and the code path was read there before a hook
was enabled; no address comes from MW2. The title's network is on through the
`lan` and `server` services only
([MW3-MP-BRINGUP.md](MW3-MP-BRINGUP.md#the-network)), where the senders are
as in the columns above, and none of this has met a hostile peer yet.

| fix | MW3 function | state | what was read there |
|---|---|---|---|
| joinParty subparty count (CVE-2019-20893) | `sub_821978C0` (`pa_joinrequest`) | fixed in IW5 | IW5 has no `joinParty`: the request is binary, and the handler refuses a count below 1 or above 18 (`cmplwi r24,0x12` at `821979F8`) before it copies that many bytes into its frame |
| Huffman decompression (CVE-2018-10718) | `sub_8229F0B0`, callers `sub_822C1F18`, `sub_8217A9F0` | wired | the loop ends on the input bits alone (`blt` at `8229F110`); the server's buffer is 0x800 bytes of its frame, the client's a 0x10000-byte LargeLocal whose only check is on the compressed size |
| relay reflection | `sub_82171930`; router `sub_8217EC58` | wired | the handler forwards to the member `argv[2]` names (`sub_8217EE68`) and never asks who sent it; the router is made to answer the direct route (its "allow an intermediary" argument cleared) |
| reliable-acknowledge freeze | `sub_822CE780`, `sub_822CE858` | wired | `sub_822C76B8` stores the acknowledge (client+0x20E68) with the signed `seq - ack >= 128` test alone, and both loops run from `ack + 1`; MW2's third, printing loop is an empty loop in `sub_822C6568` here, reached only at a difference of 129 |
| party slot count | `sub_823B07A0`, `sub_823B08C8`, `sub_823B40C0` | wired | the counts (session+0x60/+0x64) are summed by `sub_823AFF40`, which bounds walks over the 18 member records (`sub_8218BD78`, `sub_82191050`); the partystate sends them as two bytes |
| client packet sender | `sub_821766C8`, `sub_821759F0` | wired | `Netchan_Process` (`sub_822A31C8`) takes no address; `challengeResponse`, `connectResponse`, `statResponse` and the new `mruleResponse` test the state alone; the migration table (`sub_82178610`) ignores the sender. Addresses are 16 bytes here |
| `activeAction` | `sub_82165210` | wired | registered with flags 0, handed to `Cbuf_AddText` when not empty, settable by the set-dvar server command |
| command dvars from the host | `sub_82141250` | wired | every name but three goes to `Dvar_SetFromStringByName` with source 0 (`sub_823266D0`); `com_errorResolveCommand` and, new in IW5, `com_completionResolveCommand` are run as commands by `sub_822FAC08` / `sub_822FAC80` |
| map name in a `map` command | none | absent in IW5 | no `map %s` line exists in the image; `StartServer` (`sub_822EC8A0` at `822EC940`) and the party start queue a call (`sub_82282A20`) whose function passes the name as a string (`sub_822E59B8`) |
| player name as a print format | `sub_822C18A8` | absent in IW5 | the 20-second test is there (`cmpwi r7,0x4E20` at `822C18D8`) and its failure branch goes straight to the epilogue: no print, and the message is not in the image |
| disconnect reason as a print format | `sub_82171640` | wired | `821717B0`..`D8`: the localised reason goes into `EXE_SERVERDISCONNECTREASON` and the result is `Com_Error`'s format. The reason reaches the original with `%` made `.`, as the server's own filter (`sub_822A54F0`) makes it |
| playlist map index and history | `sub_822F3B88`, `sub_822F3AC0`, `sub_822F3A18`, `sub_822F3D90`, `sub_822F3E08` | wired | each adds the host's index (`go`: atoi; partystate: 8 history bytes and a vote byte) to the row's first entry and compares it with nothing; bounded by the row's own entry count (row+0x172) |
| hud element field index | `sub_822A2770` | wired | 6-bit last changed field and gaps, no compare with the 42-entry table before the tail copy; same code as MW2's, the table behind a pointer |
| baseline snapshot entity count | `sub_82179010`, `sub_82179310` | wired | the count before the array (`0x8269BE80`) is never compared; the next global is 1024 entries on, and the server stops at 0x400 (`sub_822CB6F8`) |
| entity, player state, struct field indexes | `sub_822A2370`, `sub_822A2928`, `sub_822A2118`; gap reader `sub_822A2058` | wired | each accepts a last changed field one past its table (`ble` at `822A2410`, `822A29E0`, `822A2190`); the struct reader loops until an index equals it, the player state decodes the first index not below it, the entity its first index unchecked. Kept at the gap reader, with the originals running |
| migrated game state size | `sub_8224BF90`, `sub_82328790` | wired | the file is opened with the header's length (+0x45C of `0x82FC7D00`) over the 0x180000-byte buffer; the segment opener adds up size words (`82328818`) against nothing |
| script save ids, code positions, entity pairs | `sub_823283B8` (reads at `822AD74C`, `822ACDD8`, `822AD134`, `822ACF74`, `8224AB40`), `sub_822AD328`, `sub_822AD388` | wired, less the type check | same 0x12000-byte object table (`0x833E2C60`), same signed object number, same code positions added to the program start, same 256-record list (`sub_822679E8`). MW2's object type bound protects a type name table this executable does not have |
| UI directive overflow | `sub_82304170` | wired | `stb r10,0(r29)` at `8230426C` into 0x100 bytes of the frame, no bound |

Two more of the same kind were found while reading those paths, in code MW2
does not have:

| fixed | sender | file |
|---|---|---|
| A partystate fragment's 3-bit index and count are stored by, with room for 6: an index of 6 or 7 writes up to 0x4F0 bytes over the end of the party, and a count of 7 parses a message header out of the host's own bytes (`sub_82188020`) | host | `runtime/partystate_fragment_index.cpp` |
| The snapshot's glass state indexes its 1024-piece table with 11 bits, ended only by an index of exactly 1024, or fills a run of up to 2047; past its end the message gives -1 and the loop never ends (`sub_821214A0`) | host | `runtime/glass_piece_count.cpp` |

Read and left alone: the voice packets (`sub_8217F6A8`: talker below 18 and a
party member, 1..40 frames of 10 bytes into a fixed buffer; `vt`,
`sub_8217EA08`: member index below 18, the session's nonce and the member's
XUID compared).

## Both titles, outside the game code

| fixed | sender | file |
|---|---|---|
| An integer divide by zero, or the one signed divide that overflows, ends the process: the recompiler emitted the host's divide, which traps, where the console's does not. Every guest divide now gives 0 in those cases, as Xenia's does | anyone who can choose a divisor | `patches/xenonrecomp-mw2.patch` (`PPCDivide` in `XenonUtils/ppc_context.h`) |
| The Steam lobby's `connect` line goes to the console, so text after a command separator runs as console commands. Only a line of the exact shape a host advertises is accepted | lobby owner | `runtime/online/service.cpp` (`IsConnectLine`) |
| A guest path with `..` in it reaches files above the game folder or the save package, and a map name a host picks is a guest path (it names a fastfile). Paths are read the object manager's way, as Xenia does: `..` takes back the name before it and never climbs above the device; a component the host would read as a drive is not found | host, partner | `runtime/kernel/files.cpp` (`Canonical`) |
| An indirect call to an address outside the code section reads past the function table, into memory the guest writes, and jumps to the host address it finds there. Every indirect call is bounded to the code section; outside it the address is reported and the process ends, as for any call with no recompiled function | anyone who can corrupt a guest function pointer | `tools/genshared.py` (`PPC_CALL_INDIRECT_FUNC`) |
| A disc image's file names are used as names in the game folder, so `..\x.ff` or a drive in a name writes outside it. A name holding a separator or a colon is skipped, and the root directory's size is compared with the image before it is read | whoever made the disc image | `launcher/disc.cpp` (`IsFileName`) |

## Which builds carry them

The default build (title update 6) carries every fix. The disc-version build
(`VERSION=tu0`, [building.md](building.md#the-disc-version)) carries the
multiplayer joinParty fix and the fixes outside the game code. Its address
table (`runtime/title.h`) has no entries for the other hooks, so they compile
out. The MW3 multiplayer build (`-DMW2_TITLE=mw3mp`) carries what its section
above lists as wired, and the fixes outside the game code; the MW3 campaign
build carries only those last.

A hook compiles only when its title's address table (`runtime/title.h`) has the
entries it names, so each file builds into the titles it applies to and is
empty in the others.
