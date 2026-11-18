# Modern Warfare 3 multiplayer: bring-up

The disc's second executable, `default_mp.xex`, as a title of its own beside the
campaign ([MW3-BRINGUP.md](MW3-BRINGUP.md)). The model is MW2's multiplayer
([multiplayer.md](multiplayer.md)): one runtime, a recompiled tree and a block
of `runtime/title.h` per executable.

State: it recompiles, links and packages. **It has not been run.**

## The executable

`iw5mp_ship.exe`, title id `0x415608CB`, version `0x00000002` (base version the
same), media id ending `30775D59` (the campaign's second supported packaging is
from the same disc). SHA-256
`0d7669844122b8f94e29ec87baf6bf0a7b84ab4abad8a82b43aa444682f90f7c`.
Image base `0x82000000`, size `0x1FA0000`, entry `0x824DBAE0`, `.text` at
`0x820A0000` (`0x4CA954` bytes), 15951 `.pdata` functions. Static libraries the
campaign lacks: XNET, XONLINE, XHV, XPARTY.

`tools/check_mw3_xex.py mw3/tu0/default_mp.xex` accepts this file and no other
under that name.

## One runtime, two MW3 titles

| | campaign | multiplayer |
|---|---|---|
| executable | `mw3/tu0/default.xex` | `mw3/tu0/default_mp.xex` |
| recompiler config | `config/MW3.toml` | `config/MW3MP.toml` |
| switch tables | `config/mw3_switch_tables.toml` | `config/mw3mp_switch_tables.toml` |
| generated C++ (git-ignored) | `ppc_mw3/` | `ppc_mw3_mp/` |
| tools | `MW2_TITLE=mw3` | `MW2_TITLE=mw3mp` |
| CMake | `-DMW2_TITLE=mw3` | `-DMW2_TITLE=mw3mp` |
| definitions | `MW3_TITLE` | `MW3_TITLE`, `MW3_TITLE_MP` |
| Android flavour, package | `mw3`, `com.mw3.recomp` | `mw3mp`, `com.mw3.recomp.mp` |
| Windows build | `win_build.ps1` | `win_build.ps1 -Title mp` |
| recompile only | `tools/recomp_mw3.sh` | `tools/recomp_mw3.sh mp` |

`MW2_TITLE_MP` is not defined for it: that selects MW2's multiplayer fixes.
What the network needs of the title is named in its own block of
`runtime/title.h` ([The network](#the-network)).

## Recompiling

The source of every address is the campaign's function, not MW2's: the two MW3
executables are one engine and one set of libraries built twice. A function's
first words, with branch displacements and the linker-filled halves of
immediates masked (as `tools/find_in_title.py` does between MW2's two), were
searched for in the multiplayer's `.text`; each of the following matched once,
and was then compared over its whole length.

| | campaign | multiplayer |
|---|---|---|
| `restgprlr_14` | `824BED50` | `825424F0` |
| `savegprlr_14` | `824BED00` | `825424A0` |
| `restfpr_14` | `824BEDFC` | `8254263C` |
| `savefpr_14` | `824BEDB0` | `825425F0` |
| `restvmx_14` | `824C01D8` | `82543D48` |
| `savevmx_14` | `824BFF40` | `82543AB0` |
| `restvmx_64` | `824C026C` | `82543DDC` |
| `savevmx_64` | `824BFFD4` | `82543B44` |
| `setjmp` | `824C0AF0` | `82544510` |
| `longjmp` | `824C1590` | `825452B0` |

XenonAnalyse finds 156 jump tables. The first run gave 436 "switch case outside
function" lines at 33 sites. `tools/fixbounds.py` (`MW2_TITLE=mw3mp`) derived the
leaf-function extents in `config/MW3MP.toml`; one was corrected by hand
(`0x823B2178`: it follows a `.pdata` function's tail branch with no padding, so
the tool had extended the function before it), and the three large functions
with 8 bytes of exception-handler data ahead of them got the campaign's
treatment. One more extent has no jump table: the leaf at `0x82134648` branches
with `bdzf` past the point where the analyser ended it, and its generated code
named labels it did not have. Result: 0 errors, 37683 functions, 149 source
files plus `ppc_func_mapping.cpp`, all compiled and linked into `libmw2.so`
for arm64.

The multiplayer also uses VMX instructions the recompiler did not know, all in
one stretch of code (`0x8246D6A8`..`0x824922C0`; the campaign has none, so
presumably the voice codec): `vavguh`, `vsrh`, `vslh`, `vsrah`, `vspltish`,
`vsubshs`, `vmaxsb`, `vcmpequh`, `vpkshss128`, 1615 uses. The recompiler skips
what it does not know without a word in the output, so they were added to
`patches/xenonrecomp-mw2.patch` (`recompiler.cpp` and `register_flow.cpp`). A
`XenonRecomp/` checkout made before that needs the patch applied again or the
checkout deleted. They have not been run.

## `runtime/title.h`

The multiplayer block has the campaign block's entries, below, and after them
the network hardening entries ([security.md](security.md#modern-warfare-3-multiplayer)).

| entry | campaign | multiplayer | compared |
|---|---|---|---|
| `T_ENTRY_POINT` | `82459E58` | `824DBAE0` | XEX header |
| `T_Com_Error` | `8229D688` | `8229BAE0` | first 136 instructions equal; one call fewer at the end |
| `T_D3D_Present` | `820A16E8` | `820B1B00` | equal, 1972 bytes |
| `T_D3D_ArenaWait` | `8209B5E8` | `820AB948` | equal, 180 bytes |
| `T_D3D_InitPixelShader` | `820984F0` | `820A8578` | equal |
| `T_D3D_PatchVertexFetches` | `820A4468` | `820B4898` | equal, 1572 bytes |
| `T_Image_FlushMove` | `823D4398` | `823F7A30` | equal, 592 bytes |
| `T_Cbuf_AddText` | `8228CA48` | `82282908` | equal, 276 bytes |
| `T_DATA_TimeStampBundlePtr` | `820007B0` | `820007B8` | import word `000100AD`, once |
| `T_DATA_DebugMonitorPtr` | `8200093C` | `82000960` | import word `00010059`, once |
| `T_DATA_ModuleHandlePtr` | `8200087C` | `8200089C` | import word `00010193`, once; the same three loaders |
| `T_DEV_PresentInterval`, `T_DEV_ProgressBlock` | 13872, 11024 | the same | the flip packet writer (`820B15C8`) and the arena wait read them |

`T_DATA_ModuleHandlePtr` is new: `kernel.cpp` had the campaign's address as a
constant.

Not enabled, as in the campaign: `T_Memcard_InitializeSystem` (located,
`82355E60`). Not located: `T_CG_CullZoom` (no match for the campaign's
`821067A8`), and what the campaign block lists as not located
(`T_D3D_ReplayRecording`, `T_Job_WaitPredicate`, `T_CG_ViewFov`,
`T_Dvar_RegisterInt`, `T_DB_MissingAsset`, `T_Com_Printf`). MW2's party, map
list and stats hooks have not been looked for: they are compiled out.

The network hardening entries were not found by matching MW2's code (its
multiplayer executable is not an input here) but by reading this executable:
the out-of-band command tables (`0x82014E64` the migration messages,
`0x82016468` the party client's, `0x820181B8` the party host's, `0x82018550`
the party migration's), the callers of the message readers, the strings that
survive in a ship build, and the globals each function reads. The evidence for
each is in `runtime/title.h` beside the entry and in security.md's table.

## The network

The title's network goes through the online service MW2's multiplayer uses
([multiplayer.md](multiplayer.md#the-online-service)), with two of its
backends: `lan` and `server`, a relay ([relay.md](relay.md)). The hardening
([security.md](security.md#modern-warfare-3-multiplayer)) is what stands
between a peer and the title's parsers. **None of this has been run**: it
compiles, and every address below was read in the executable.

What was MW2's, and what MW3's is:

| | MW2 | MW3 multiplayer | where it was read |
|---|---|---|---|
| title id (`XNetLogonGetTitleID`, Live storage paths, invitations) | `0x41560817` | `0x415608CB` | the XEX header; the import's one caller is `sub_825108E0` |
| protocol number (`T_ProtocolVersion`) | `sub_8230A3D8`: 136, 128 on the disc | `sub_82320FD0`: `(sub_82286A88() << 16) + 0x56` | callers: the client's connect (`821748B8`), `SV_DirectConnect` (`822C2604`), `pa_joinrequest` (`82197960`), the server's `infoResponse` (`822C7138`) |
| its high half | the online update's version | the same (a halfword at `0x83015C70`), or -1 or -2 without an update, by the copy's state | `sub_82286A88` |
| the refusal (`T_NET_OutOfBandPrint`) | `sub_8228EE28`, version text "1.4" | `sub_822A3D40`, "1.0" | `822C2610`..`34`: `error\nEXE_SERVER_IS_DIFFERENT_VER\x15%s\n` |
| `XSESSION_INFO` | 60 bytes | 60 bytes: the console's | the session keeps it at +0x14, copied with a length of `0x3C` at `823B0874`; the nonce at +0x58 |
| `XSessionCreate` | `000B0010`, 28 bytes | the same seven words | `sub_82511270` (`li r7,0x1C` at `8251138C`) |
| the other session messages | `000B0006`, `07`, `10`..`15`, `18`, `1C`, `1E`, `1F` | the same, each at least as long as its handler reads; also `08`, `1B`, `21`, `25`, `65`, which answer success | the callers of `XMsgStartIORequest` (`0x825699E4`) |
| Ethernet link | up in the multiplayer build | up when a service started | the test is `sub_8234F3A8` (`XBOXLIVE_NETCONNECTION`) |
| System Link search | `getinfo xxx` to port 1000, broadcast | the same | `sub_82173AB0` |
| what copies send each other | `MW2L`, `MW2P` packets, `MW2O` in the `XNADDR`, `mw2recomp` | `MW3L`, `MW3P`, `MW3O`, `mw3recomp` | `T_NET_TAG`, `T_NET_NAME` in `runtime/title.h` |

`runtime/network_version.cpp` puts the release in the protocol's high half, as
in MW2; here that also takes out the -1 and -2, which two copies in different
states would have refused each other over.

**System Link is where players meet.** A host's game is found by the title's
own broadcast, which the service carries to everyone it reaches: the local
network on `lan`, everyone on the relay with the same title and protocol on
`server`. So on a public relay the System Link list is the list of public
games, and anyone can host one.

A game is in the list once its host has **started the match**. The screen
after Create Game, with START GAME, is the host's own: nothing is hosted yet.
The list is filled by `getinfo` (the client's `sub_82173AB0`), which only a
running server answers (the packet loop `sub_82299D50` hands packets to the
server at `82299DC8` and `82299E78` behind its running flag), and only with
the `systemlink` dvar set (`sub_822C7470` at `822C7554`..`78`, which calls the
`infoResponse` writer `sub_822C7040`). The server's session, flags `0x21`, is
made as its map starts (`sub_822C5548` at `822C5808`, when `onlinegame` or
`systemlink` is set), and the log then says `online: session ... created`
and `online: hosting`. The answer carries the session's `xnaddr`, `xnkid` and
`xnkey`, which the client's handler (`sub_821775E8`) reads. This has not been
seen on a device.

`MW2_TRACE_NET=1` logs what the title asks of its sessions and sockets and
what the service does with each datagram, as `net-trace:` lines, and every
five seconds that the title's main thread is alive. A debug build of the app
reads more switches from `options.txt` in its external files folder, one
`MW2_NAME=value` a line.

**Xbox LIVE is off.** The executable carries Activision's DemonWare client
(`code_source\DemonWare\...`, the `MW3 - DW ATVI LSP` title server) beside
Live: the functions that raise `EXE_DEMONWARE_DISCONNECT` (`sub_823D54E8`,
`sub_823D7F28`) end an online game whose player it is not connected to, and
nothing here stands in for those servers. So the player is signed in locally,
as with no
service, and a session asking for Live features is refused. `MW2_LIVE=1` (the
app's "Sign in to Xbox LIVE (experimental)") signs the player in to Live as in
MW2, to see how far the menu gets; the relay's Find Game listings and the
`lan` invitations are behind it.

Which service:

- `online/service.cpp`: `MW2_NETWORK=lan` or `server`; anything else, or
  nothing, is none. Steam is not offered.
- With none, `kernel/net.cpp` keeps the earlier guard: the title's sockets are
  bound to 127.0.0.1, a send to any other address is dropped, the link reads as
  down, and `connect` is refused (that one with a service too).
- The app (Settings, NETWORK): Online, the default, is `server` with the relay
  address given (`Prefs.DEFAULT_RELAY_ADDRESS` is the one place a default is
  written; with no address Online is `lan`); LAN is `lan`; Off is none. The
  player's name is `MW2_NAME`. A public relay takes no password.

### Two copies on one device

`win_build.ps1 -Title mp -Instance 2` (gradle `-PmpInstance=2`, debug only)
builds the same app as `com.mw3.recomp.mp2`, "CoD: MW3 Multiplayer (2)",
`out\mw3mp2-debug.apk`. It has its own settings, its own profile (so another
player; its name starts as "Player 2") and its own game folder: the game is
installed in it from the disc image again.

Both copies use the same ports, on `lan` and on a relay alike. Each takes the
first free port above `MW2_LAN_PORT` (3074) as its own, so the second to start
gets the next one and is a second player; on `lan` both also listen on 3074
itself, shared, for broadcasts. What a copy says to everyone it also sends to
the first four own ports at its announced address, so the two find each other
with WiFi off as well.

Before trusting it with strangers: a local match with a service on (the
log's `online:` warnings should stay silent), then the two copies on `lan`,
then through the relay.

## Everything unlocked

`MW2_UNLOCK_ALL=1` (the app's "Unlock all camos and items", Settings →
Graphics → Multiplayer, on by default) makes every item read as unlocked.
`runtime/mw3_unlock_all.cpp` hooks the title's own checks and what the menus
read of a weapon's level; the stats (rank, prestige, weapon levels,
challenges) are not written, and a match counts experience from what was
earned, so switching it off shows that again.

| function | what it is | evidence |
|---|---|---|
| `8228FA68` (stats, table, row) | decides a row of `mp/unlocktable.csv` | reads the row's type (column 1) and compares it with `title`, `icon`, `weaponAttachment`, `weaponCamo`, `weaponBuff`, `weaponReticle`; the weapon ones go to `8228AC18`, which compares the stats' `weaponRank` of the weapon with `mp/weaponRankTable.csv` (columns 2 to 5); the rest compare the rank from `experience` and a challenge. 15 callers: the menu's four functions below, the script's `isItemUnlocked` (`82255688`), the challenge and custom class checks, and the copies into the profile's bits (`82182748`, `821829C0`) |
| `8228FE60` (controller, table, row) | the menu's way into it | fetches the controller's stats (`82289F88`) and answers 0 without them. Called by `isItemUnlocked` (`822D6B10`), `isCardIconUnlocked` (`822D6D38`), `isCardTitleUnlocked` (`822D6E60`), `isProfileItemUnlocked` (`822D73B0`) |
| `821829A0`, `821829B0`, `82182AA8` | the profile's `iconUnlocked`, `titleUnlocked` and `unlock` bits | three-instruction leaves that load the name and jump to `82182610`; the menu functions call them when the stats are not the source (`82288968` returns 0) |
| `822D6120` (controller, operands, first, result) | the menu's `getPlayerData` | for a path starting with `weaponRank` the result is raised to the highest level in `mp/weaponRankTable.csv` (the largest number of its column 4, which `8228AC18` compares the stats' `weaponRank` with), for `weaponXP` to the largest number in the table. The table is read at the first such call (`82315A68`), and the log says `unlock: the menus read every weapon at level N` |

The menu's `isItemUnlocked` already answers 1 for everything in a private match
(`xblive_privatematch`, tested at `822D6B74`), and 1 for a name the table does
not have. A camouflage is asked for as `iw5_<weapon> <camo>` (`8228AD68` splits
the row's name, and raises "Incorrect format for unlockable weapon item"
otherwise).

On the device the first three rows opened Create a Class at rank 1 and left an
M4A1's attachments and proficiencies locked: the menus compare the weapon's
level themselves, and the executable has no menu function for it, so they read
it with `getPlayerData`. That reader is the fourth row; it has not been run.
The weapon level a menu shows is therefore the highest for every weapon, and
the script's reader (`8225A710`) still gives the earned one.

Not covered: a menu that reads the bits an entitlement sets (`unlockedCamo`,
`unlockedReticles`, written by `823C6980` and `823C6AC8`; no unlock check in
the executable names them). The rank and prestige shown are the real ones.

## Installing and starting

The app copies `default_mp.xex` (checked against the hash above) and the game
files from the same disc image the campaign app takes; it does not look at
`default.xex`.

    powershell -File scripts\win_build.ps1 -Title mp      # out\mw3mp-debug.apk
    adb install -r out\mw3mp-debug.apk
    adb shell am start -n com.mw3.recomp.mp/com.mw2.recomp.MainActivity

## Risks at first boot

- Nothing here has run. The campaign's first failures are the likely ones
  ([MW3-BRINGUP.md](MW3-BRINGUP.md#4-boot-on-the-desktop)).
- Imports the campaign never calls (XNet, XOnline, XParty, XHV, sessions,
  matchmaking) are logging stubs that return 0 (the list is in
  `ppc_mw3_mp/stubs_generated.cpp` after a build). A title that takes 0 from
  one of them for success and then waits on it will hang at start-up.
- With no service the link is down, and the menu may refuse everything but
  split screen.
- The network: see [The network](#the-network). A System Link game between
  two copies has not been tried, on `lan` or on a relay.
- Zones: the multiplayer loads `*_mp.ff` files, which must be on the disc image
  used; nothing checks for them before the title asks.
- The heap starts at `0x85000000` for both MW3 titles; the multiplayer's image
  and lookup table end at `0x84956848`, below it.
