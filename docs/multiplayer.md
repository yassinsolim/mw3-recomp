# Multiplayer

The disc carries two programs. `default.xex` is the campaign and special ops;
`default_mp.xex` is the multiplayer, a separate build of the same engine with its
own fastfiles, menus and maps. The runtime is the same for both; what differs is
the recompiled tree it is linked against and the table of guest addresses it
hooks.

    TITLE=mp ./build.sh
    MW2_CONSOLE="8:map mp_afghan" ./build-mp/mw2 mw2/default_mp.pe mw2/game

## One runtime, two titles

| | campaign | multiplayer |
|---|---|---|
| executable | `mw2/default.xex` | `mw2/default_mp.xex` |
| flat image | `mw2/default.pe` | `mw2/default_mp.pe` |
| recompiler config | `config/MW2.toml` | `config/MW2MP.toml` |
| switch tables | `config/mw2_switch_tables.toml` | `config/mw2mp_switch_tables.toml` |
| generated C++ | `ppc/` | `ppc_mp/` |
| build directory | `build/` (`build-release/`) | `build-mp/` (`build-mp-release/`) |

Both are the executables title update 6 makes of the disc's; the disc's own
are a build option with names of their own
([building.md](building.md#the-disc-version)).

`build.sh` takes `TITLE=sp` (the default) or `TITLE=mp`. CMake takes
`-DMW2_TITLE=sp|mp`, which selects the recompiled tree and, for `mp`, defines
`MW2_TITLE_MP`. The Python tools make the same choice through the `MW2_TITLE`
environment variable (`tools/title.py`), which reads the PE section table from
the image because the two executables lay their sections out differently.

`config/MW2MP.toml` has its own `[main]` block: the multiplayer's addresses of
the register save/restore helpers and of `setjmp`/`longjmp`, and its own list
of jump-table functions (`tools/fixbounds.py`).

### `runtime/title.h`

Every function in the two images sits at a different address. `runtime/title.h`
is the only place the runtime names guest addresses; everything else refers to
`T_*` entries through macros:

| macro | expands to |
|---|---|
| `GUEST_FUNC(T_x)` | the recompiled function, `sub_<addr>` |
| `GUEST_ORIG(T_x)` | the original once hooked, `__imp__sub_<addr>` |
| `GUEST_HOOK(T_x) { ... }` | declares the original and defines the hook |
| `GUEST_ADDR(T_x)`, `GUEST_NAME(T_x)` | the address as a number, and as a log string |

A function entry is bare hex so it can be pasted onto `sub_`. A data entry
(`T_DATA_*`) is an ordinary constant and is zero where the address is not known
for a title; code that reads one checks for zero. A function not located in one
title has no entry there, and the hook naming it is compiled out under `#ifdef`
(`T_DB_MissingAsset` in the multiplayer). The client-state offsets used by the
walk autopilot are known only for the multiplayer, since the campaign accepts
`setviewpos`; the save-device table is known only for the campaign.

### `tools/find_in_title.py`

Locates the other executable's copy of a function by code, not by name. The two
images are one engine linked twice, so a function's instructions match except
for what the linker filled in: branch displacements and the 16-bit halves of
absolute addresses. Masking those out of the first N words gives a signature;
a unique match with the same `.pdata` size is the same function.

    tools/find_in_title.py 820C3390 8227CF18     # campaign -> multiplayer
    tools/find_in_title.py --table               # every T_ entry in title.h
    tools/find_in_title.py --reverse 823FF900    # multiplayer -> campaign

Several matches mean the first words are a common prologue; `--words` uses more.
A unique match of a different size needs reading before it is trusted.

## What the multiplayer needs from the runtime

**Every fastfile.** The campaign loads the level it is told to; the multiplayer
chooses its own map and zones. A zone whose file is missing makes the title
report a dirty disc and fail, so `build.sh` and the launcher extract every
`.ff` and `.pak` on the disc (about 5.9 GB), and the campaign's Bink movies
beside them ([gameplay.md](gameplay.md#movies)); the multiplayer plays none.

**A network link.** The multiplayer refuses to start any match without one, and
a system-link match is how it reaches a map. `XNetGetEthernetLinkStatus`
(`runtime/kernel/net.cpp`) reports an active 100 Mb/s full-duplex link in the
multiplayer build and no link in the campaign build; `MW2_NET_LINK=0|1`
overrides either.

**Sessions through XAM.** The title's session, matchmaking and statistics calls
reach the kernel as XAM messages through `XMsgStartIORequest`, not as imports of
their own. `XSessionCreate` is app `0xFB`, message `0x000B0010`, with a 28-byte
buffer `{ session, flags, public slots, private slots, xuid, session info out,
nonce out }`. `runtime/kernel/xam.cpp` fills the 60-byte `XSESSION_INFO`: an
8-byte session id, the 36-byte `XNADDR` from `XNetGetTitleXnAddr`, and a 16-byte
key. It must be filled: the title reads the session back, and an all-zero id is
treated as a Live error (`Com_Error(XBOXLIVE_LIVEERROR)`). A session id's top
bits mark its kind: clear for system link, `0x80` for a Live session. A session
asking for Live features (presence, stats, matchmaking, arbitration) is refused
with `X_ONLINE_E_SESSION_NOT_LOGGED_ON` unless the online service carries
lobbies, and the title then takes its offline path.

**Occlusion queries.** The renderer issues them (the sun glare test) and a job
worker polls for the result, so `EVENT_WRITE_ZPD` must always write its
sample-count record. See [rendering.md](rendering.md).

**No `setviewpos`.** The multiplayer refuses it, so a spot is reached by walking
there (`MW2_WALK_TO`, `MW2_WALK_PATH`; [switches.md](switches.md)).

## The online service

What the title's network calls reach is a backend the player chooses as the
game starts, declared in `runtime/online/service.h`. The kernel side (`kernel/net.cpp`,
`kernel/xam.cpp`) keeps everything Xbox-shaped -- `XNADDR`s, session keys,
sockets, ports -- and asks the service only for what a service does:

| call | purpose |
|---|---|
| `Start()` | log in; false leaves the title offline |
| `LocalId()` | where other players reach this one; unique and stable for the run |
| `Account()` | who the player is, stable across runs; the XUIDs are built from it |
| `LocalName()` | the name other players see (the title shows 15 characters) |
| `Send`, `Broadcast`, `Receive` | unreliable datagrams, addressed by the title's own port numbers; broadcast reaches everyone who could join, never self |
| `Advertise`, `NextJoin` | optional: system-link invitations as the title's `connect` console line |
| `Friends`, `InviteFriend`, `InviteFriends` | optional: the friends screen's list and one invitation; or, without a list, everyone invited at once |
| `Lobbies`, `SetJoinable`, `NextInvite` | optional: Xbox LIVE parties and private matches |
| `Matchmaking`, `List`, `Search`, `SearchResults` | optional: Find Game |
| `TitleFile` | optional: a file of the title's every player must share (the playlists) |

    MW2_NETWORK=none     # default: system link on this machine
    MW2_NETWORK=lan      # plain UDP between machines
    MW2_NETWORK=server   # through a relay server (MW2_SERVER)
    MW2_NETWORK=steam    # through the player's Steam client, or lan without it

The launcher's NETWORK screen writes it, with the server's address and
password, to `.env`. Every build has every backend; a backend is a function
that makes it (`online::MakeLan`), and `online::Get()` tries the chosen one's
list in `service.cpp` and keeps the first whose `Start()` succeeds: `steam` is
Steam and then lan, so a player whose Steam is not running plays over the local
network instead, as another player (below). The log names the service. Only
steam joins the Flatpak Steam's namespace at the start
(`EnterServiceNamespace`).

Modern Warfare 3's multiplayer uses the same service with `lan` and `server`
only, and is not signed in to Live: its players meet in System Link
([MW3-MP-BRINGUP.md](MW3-MP-BRINGUP.md#the-network)). The Android apps set
`MW2_NETWORK` from Settings, NETWORK: Online (a relay, or the local network
when no relay address is given), LAN or Off.

With `none` there is no service, and the title's sockets are host UDP sockets, enough
for system link on one machine. Only one copy can have the title's ports then;
a second one's binds fail, which the title takes for a fatal error and restarts
on, so `NetDll_bind` leaves its sockets unbound and it plays without a network.

The player's rank is the profile's, whichever service he plays through
([gameplay.md](gameplay.md#signing-in)): the offline stats under his offline
XUID and, signed in to Live, the Live stats under his online one
([saves.md](saves.md)).

With a service, the title's sockets are the service's and nothing is
bound on the host, so several copies can run on one machine. A peer's 64-bit id
travels inside its `XNADDR` (in `abOnline`, behind the tag `MW2O`), and
`XNetXnAddrToInAddr` hands the title a stand-in IPv4 address in `10.0.0.0/8`
for it. Datagrams to a stand-in address or to broadcast go to the service;
datagrams to this machine's own address come straight back.

`online::Frame()` runs once a frame on the title's main thread (from
`XamInputGetState`). It hands an accepted Live invitation to the kernel, or, for
a system-link join, types the commands the System Link menu would run followed
by the host's `connect` line.

A system-link host logs the line another machine joins with:
`online: hosting; another machine joins with "connect <xnaddr> <key> <id>
<public> <private> <nonce>"` -- the same command the title's server browser
issues after a search. `MW2_TRACE_ONLINE=1` logs every datagram through the
service (diagnostic builds).

### LAN and server

The two are one service, `runtime/online/players.cpp`, over two transports
(`transport.h`): a transport opens, says where this copy is reached and which
copy on the machine it is (its slot), sends to one peer or to all, and hands
over what arrived. The service frames the title's datagrams, keeps who is
there, and carries the Xbox LIVE lobbies, so both have the same System Link
list and private matches.

`runtime/online/lan.cpp`: plain UDP on the local network. Each copy sends from a
port of its own, the first free one above the shared port; broadcasts go to the
shared port, `MW2_LAN_PORT` (3074), and, off Windows, to the first four own
ports at this machine's announced address, so two copies on one machine find
each other with no network connection at all. A peer id is the IPv4 address in
the top bits and the port in the low 16. The log names the network connections
found and the one announced.

`runtime/online/server.cpp`: a relay server (`relay/`, [relay.md](relay.md)) at
`MW2_SERVER`, with `MW2_SERVER_PASSWORD` (a public relay asks for none). The
copy says Hello with its title id and protocol number
([Versions](#versions)), answers the
relay's challenge with the token it was given, which every later packet
carries, and gets its id, 48 bits; one that has none within five seconds does not start,
and the title stays offline. "To all" is everyone on the relay with the same
title and protocol, and every
packet goes through it, so nobody needs an open port and nobody learns another
player's address. A thread says Alive every second, so a copy stays known while
the title is loading a map and not polling; a relay that forgot it answers
Unknown, and the copy says Hello again with the id it had. Its slot is counted
as lan's is, by the first free port above the shared one, which it holds and
sends nothing from: what goes to the relay leaves from a port the system
picks. Every first copy's slot port is the same (3075), and a second machine
behind one router that came to the relay from it got no answer from a router
that could not give it another ("did not answer within 5 seconds", seen from
a PC while an AYN Thor on the same network was in a match).

The account is the profile at the first controller
([gameplay.md](gameplay.md#signing-in)) plus the copy's slot, so a second
copy on one machine is a second player; `MW2_NAME` is another name for him than
the profile's. The log says so when another machine has the same account.

A copy forgets a player it has not heard for ten seconds, and keeps 256 at
most: on a public relay anyone can announce any name. The Android HUD's "LAN"
number is how many it hears.

Every copy announces who it is and what can be joined once a second to
everyone the transport reaches. `MW2_LAN_JOIN=<name>` joins that player's party or lobby as soon as
it is seen, as choosing a friend and "Join Session" does on the console. The
lobby's "Invite friends" invites every player seen; a copy started with
`MW2_LAN_ACCEPT=1` accepts.

### Steam

`runtime/online/steam.cpp` talks to the Steam client the player already runs.
It loads the client library from the install -- on Linux the one the running
`steam` process has its `steamclient.so` mapped from, else the first of the usual
location, the snap and the Flatpak; `steamclient64.dll` from the registry's
`SteamPath` on Windows; `MW2_STEAM_ROOT` names the install instead -- and calls its
versioned interfaces through their vtables, as `libsteam_api` does. Nothing of
Valve's is built in or shipped; the Steamworks SDK is a reference for slots and
structures only. Another install's library than the running Steam's connects
halfway, then fails its first calls (`IClientUtils::GetSteamRealm returned
failure code 12`) and crashes.

**The Flatpak's Steam** runs in a process namespace of its own and tells its
clients apart by process id: a client outside it has its connection dropped. So
when the running Steam is in another process namespace, the game joins it
first thing in `main`, before any thread exists
(`online::EnterServiceNamespace`): `setns` through a pidfd into its user and
process namespaces together (the user namespace is the player's own, so no
privilege is needed), or one after the other on kernels that refuse both at
once. A process namespace takes in only children made after joining, so the
game forks: the child goes on as the game, and the parent waits, hands it
SIGTERM and SIGHUP, and exits with its status. The mount and network
namespaces stay the host's.

The game runs as Spacewar, app 480 (`MW2_STEAM_APPID`). The app id is put in the
environment before the library loads, because the client reads it at load time
and its networking has no interface to give otherwise. Environment variables the
client adds for SDL are taken back, since they would hide every physical
controller from a game Steam did not start.

- Players are SteamIDs; datagrams are `ISteamNetworkingMessages`, with the
  channel carrying the title's port.
- Every player starts in a friends-only lobby of their own. The lobby is what
  the title's broadcasts reach, so a game a member hosts shows in every other
  member's System Link list. Networking sessions are accepted only from lobby
  members, since Spacewar is shared by everyone, except by a host and in Find
  Game (below).
- System Link is also shared with every Steam player. A player hosting a
  system-link game is listed: an invisible lobby of its own (`mw2recomp` = the
  release's number (Versions, below), `host` = its SteamID; 250 members, since a search never
  returns a full lobby). It is listed while the title's hosted session is on,
  from its creation (SV_SpawnServer) or XSessionStart to its XSessionEnd or
  XSessionDelete -- a map change ends one and creates the next. A broadcast
  from a player who hosts nothing also goes to every host listed, worldwide;
  Steam is asked for the list at most every 2 s (a listing Steam would not
  make is asked for again every 5 s). The title broadcasts only to search for
  system-link games and to answer a search. What a stranger reaches:
  - a host, while it hosts: it accepts anyone's session, and its broadcasts
    reach the strangers whose sessions are open;
  - a player who searched, on the ports its searches went to, from a host it
    found, for the rest of the run. Anyone can make a listing, with any `host`, so a listed host is a
    stranger like any other; a stranger can also join a listing and, once its
    host leaves, keep it listed under the host's id (there is no kick);
  - a player who chose to join a listed host's game (sent to it on its own),
    for the rest of the run;
  - a player listed for Find Game, while it is, which is from opening Find
    Game, with no public slots yet;
  - a player in a Find Game game (`InGame`). A party's members reach each other,
    any can be the next host, and the title tells each copy only some of their
    addresses, so such a game takes anyone's link. This lasts until the title
    deletes the session, about 40 seconds after leaving the match: it queues
    the delete. A friend's private match gives its invited players the same
    session flags, so the host marks a Find Game session in the last bit of its
    id. Every player has the id, however they came, and it stays through a
    migration; a session without the mark takes no stranger's link and is not
    listed, whoever comes to host it.
  Everything else from a stranger is dropped as it arrives (`Reaches`), so a
  player who stops hosting is closed to the strangers it played with. Their
  sessions are not closed: a host's map change ends its game and starts the
  next at once. Xbox LIVE parties and private matches stay among friends.
- The advertised game is the newest hosted system-link session that is on;
  `kernel/xam.cpp` (`PublishHosted`) chooses it and tells the service under one
  lock, so threads ending one session and creating another cannot reorder it.
- F6, or the Xbox LIVE lobby's "Invite friends", opens the friends screen
  (`friends.h`), drawn over the frame as the sign-in screen is, over a blurred
  and darkened copy of the game (`gpu/vulkan/backdrop.h`): the friends
  running this game, with their avatars, then how many others are online and
  offline. A friend counts as running it when Steam says they play app 480 and
  their rich presence has `mw2recomp`, which every copy sets to its executable
  and release (`mp:<release>`, `sp:<release>`); the rest of Spacewar's players
  have no such key. A friend in the other executable, or on another release,
  is listed but cannot be invited: the campaign's Spec Ops invitations and the
  multiplayer's do not carry into each other. Steam's overlay would need
  Steam to have started the game. The list is built on the callback thread,
  once a second while the screen asks for it, and A sends Steam's lobby
  invitation (`InviteUserToLobby`), which the friend accepts in Steam's chat; a
  friend can also be invited from Steam's own friend list. Accepting moves the
  friend into the host's lobby. With another service the screen says that
  choosing friends needs Steam; LAN still invites everyone on the network.
- The host's lobby data carries what joins its game: `xsession`, the Xbox LIVE
  party or private match the host is in, which the friend's copy takes as an
  accepted Live invitation; otherwise `connect`, the host's system-link join
  line, which the friend's copy types. Either is acted on once per lobby
  entered, and again when the friend accepts another invitation to the lobby
  they are in: the host may have started another party since, or the friend
  left it.
- Each copy logs the Steam links to the players it talks to: a request for one
  and whether it was taken, a link connecting, connected (with its ping), and
  ending (with Steam's reason), and, at most every 5 s, a link that loses
  packets or queues what it sends.
- Steam cannot launch the game from an invitation (it would start Spacewar), so
  the friend must already be running it.

#### Two accounts on one machine

Two Steam installs, each logged in to its own account, can run side by side, so
one person can test Steam play with a copy on each: the snap and the Flatpak, for
example, keep separate data and logins.

Every Steam listens for its clients on `127.0.0.1:57343`. The Flatpak shares the
host's network, so whichever Steam starts second is left without that address:
it logs in as usual and reports nothing, but a game on it reaches the other Steam
and crashes in `steamclient.so`. The environment variable
`steam_master_ipc_name_override` gives a Steam and its clients another address.
Both must have the same value, and it must be set when that Steam starts: one
started without it has to be restarted.

    flatpak run --env=steam_master_ipc_name_override=127.0.0.1:57344 com.valvesoftware.Steam

A copy on that Steam names the install and the address; a copy on the other names
only its install. `MW2_STEAM_ROOT` keeps each copy on its own Steam, library and
process namespace alike. Each runs in a folder of its own, as its saves are
relative to it, and has the other options as usual:

    MW2_NETWORK=steam MW2_STEAM_ROOT=~/.var/app/com.valvesoftware.Steam/.local/share/Steam \
      steam_master_ipc_name_override=127.0.0.1:57344 mw2 ...
    MW2_NETWORK=steam MW2_STEAM_ROOT=~/snap/steam/common/.local/share/Steam mw2 ...

The log's `online: steam as "<name>"` says which account a copy reached. The
accounts must be Steam friends for an invitation. The two copies talk through
Steam on one machine, so this tests invitations, lobbies and host migration, not
a poor connection between players.

## Versions

Only copies of the same release play together, by the title's own check
(`runtime/network_version.cpp`). Its protocol number (`sub_8230A3D8`, 136; 128
on the disc) carries the console's online-update version in the high half;
that update never runs here, so the high half carries the release instead:
`version::Network()`, 15 bits of a hash of the release tag (`MW2_RELEASE_TAG`),
or of `dev` for a build without one. The low half stays the title's.

- System Link leaves a game of another protocol out of the list
  (CL_ServerInfoPacket).
- A host refuses a client of another protocol (SV_DirectConnect) with
  `EXE_SERVER_IS_DIFFERENT_VER`, "Server is a different version: &&1". The
  title fills in "1.4", every copy's build number; the runtime puts the host's
  release there.
- A party host refuses a member of another protocol
  (PartyHost_HandleJoinPartyRequest); the member sees "Unable to join game
  session", the one message the title has for any refused party join.
- The Steam listing's `mw2recomp` value is the same number, with `-tu0` on the
  disc version, so a search finds only games it can join.

The campaign's co-op has no version check.

## Find Game

PLAY ONLINE's Find Game works on the server service, where every player is on one
relay ([relay.md](relay.md)), and on Steam. On `lan` a search finds nothing and
there are no playlists to choose from.

The title's matchmaking is Live's, not IWNet's: `iwnet_searching` is off, so
LSP stays unused. It goes:

1. `xstartlobby` takes the chosen playlist and creates a lobby's session with no
   public slots.
2. `XSessionSearchEx` (`000B001C`) asks for 50 sessions. The search's meaning
   was on Live's servers, so `kernel/xam.cpp` takes what the title passes: its
   contexts and integer properties must all match, and the room for the party
   (`0x1000001A`) is a lower bound. They are the game mode, ranked or not, the
   playlist, the protocol (so a search finds only its own release), the map packs
   and the playlists' version. The skill bounds are doubles, and are left out.
   The title sends procedure 8 or 9 and fills them in alike.
3. `XNetQosLookup` is answered from the search, at once: the round trip,
   and a wide line. The title reads nothing else of a probe, and drops a host over
   its limit, which starts at `ping_default_min` (50 ms) and rises by
   `ping_increment` every `ping_searches_per` searches.
4. It joins the nearest host over the party's own protocol, the one private
   matches use, or, after `session_advertiseFailedSearchCount` failed searches,
   opens its own lobby's public slots and goes on searching.
5. With a lobby of `party_minplayers`, open `party_minLobbyTime`, the host starts
   the match, and a player who finds it running joins it.

What a host lists comes from `XUserSetContext`, `XUserSetProperty` and the
session: the newest it hosts with matchmaking and stats, which leaves out the
party's own session (`0x29`). `XSessionJoinRemote` and `XSessionLeaveRemote` count
the players in it. Once started, a session that takes nobody (`0x400`) leaves the
list. A search takes a moment, so it stays pending on its overlapped block,
finished by `online::Frame`, and `XMsgCancelIORequest` withdraws it, since the
title reuses the block.

The title update's playlists are named `playlists.patch2`, the disc's
`playlists.info`; both come from the relay ([relay.md](relay.md#playlists)), or
on Steam from the game (below). A relay that has none for the title leaves
each copy the list its release was built with (`relay/playlists.info`,
`relay/playlists_mw3.info`, `relay/playlists_cod4.info`), as on Steam: the
relay lets only copies of one release meet, so they have the same bytes. The
table's walk stops at the first playlist with no entries, so numbering starts at 0.

`XUserReadStats` returns nothing, so the title balances no teams by skill.

### On Steam

Nothing stands between the players, so `steam.cpp` does what a relay does:

- A game is listed in an invisible lobby of its own, apart from System Link's:
  `mw2game` = the release's number, `host` = its SteamID, and `game` = the
  session, its slots and its numbers, in one value. It is made the first time the
  player hosts something to list, and is searchable while there is one.
- A search asks Steam for those lobbies, up to 200, and applies the relay's rule
  itself. Steam answers one list at a time, so it waits for a System Link search,
  and the other way round.
- The round trip is measured, not estimated: the search opens a link to each host
  it found, with one message on a port no title uses, and takes the link's ping.
  This also finds the route before the player joins. A host with no link within 4
  seconds cannot be reached and is left out. A list Steam has not given within 10
  seconds counts as empty.
- The playlists are built into the game from `relay/playlists.info`, so a release
  has one list, and a search finds only its own release.

What a stranger reaches is in [Steam](#steam) and [security.md](security.md).

## Host migration

The title moves a Live party, a lobby or a running match to another player by
itself. Nothing in the runtime decides who hosts; `kernel/xam.cpp` answers
`XSessionMigrateHost` (`000B001E`) and the title does the rest over its own
packets, which reach every player the way its game traffic does. The system
version says `0x200ED800`, the lowest with both this call (`0x200CE900`) and
`XSessionModifySkill`, since the title sends neither on an older one. A
system-link game has no party, and ends with its host.

`XSessionMigrateHost` changes only the host's address: the session keeps its id
and key. The new host passes a player's index and gets the session's info back;
it sends that to the others (`sessinfo`), who pass it in with no player. The listing
moves by itself, since a copy lists what it hosts ([relay.md](relay.md)). On
Steam the old host gives the new one the lobby too (`SetLobbyOwner`), since a
friend invited to a lobby joins its owner's game.

A lobby moves whenever the title finds a better host, so the player who opened
it may not host the match: a host can change seconds after a lobby fills.

What happens to a running match depends on how the host goes:

| | what the title does | |
|---|---|---|
| Ends the game from the pause menu | `hostmigration_start`: the players rate each other, the server streams its game state to the best one (`mstart`, `mhead`, `mdata`), and everyone reconnects to it | the match goes on without the old host, who is back at the Xbox LIVE menu; a spawned player stays where it was |
| The title's own "we suck as host" check | the same | the match goes on |
| Leaves with `disconnect`, crashes, or loses the connection | no state was sent. Players notice after 12 seconds without a packet from the host, choose a new one among themselves (`findbest`, `nominee`, `migrateto`) | the match is lost; the lobby goes on and starts another |

The stream needs the old host alive, so the last row cannot be improved from
outside the title. `migration_limit` and `migration_timeBetween` end a match
that migrates too often.

`XSessionModifySkill` (`000B001F`) comes with the same version. It succeeds and
keeps nothing, as nothing here keeps a skill.

## Xbox LIVE private matches

A service whose `Lobbies()` is true (lan and steam) has the player signed in to
Xbox LIVE, which opens PLAY ONLINE: the party, PRIVATE MATCH, the lobby, START
GAME. No Live service is involved; `runtime/kernel/xam.cpp` answers what the
title asks:

| | signed in to Live | offline (`none`) |
|---|---|---|
| sign-in state | 2, Live-enabled | 1, local |
| XUID | online `0x0009` over the account's low 48 bits; offline `0xE0...` over the same bits | offline `0xE0...` |
| privileges | all granted | all denied |

Signed in, every new notification listener for the Live area hears
`XN_LIVE_CONNECTIONCHANGED` (connection established), since the connection came
up before the title was listening. `XOnlineGetNatType` reports an open NAT.

**Sessions** are kept by session object. The newest one this machine hosts with
presence and invitations enabled -- the party's in the Xbox LIVE menu, the
private match's in its lobby -- is the joinable session; its 60-byte
`XSESSION_INFO` is handed to the service (`SetJoinable`) whenever sessions
change. The host logs the console line that joins it:
`online: joinable; another machine joins with "xpartyjoin <xnaddr> <key> <id>"`.

**Invitations** the service returns (`NextInvite`) become the console's accepted
invitation: `XN_LIVE_INVITE_ACCEPTED` is posted, then `XInviteGetAcceptedInfo`
returns it. One accepted before the title listens waits for its first Live
listener.

**Live storage** is on this machine under `saves/online/` (see
[saves.md](saves.md)). The title names files by server path
(`//title.<id>/t:<id>/<file>`, `//tuser.<id>/u:<xuid>/<id>/<file>`):

| directory | contents |
|---|---|
| `title/` | the title's own files -- message of the day, playlists. Each reads as empty, unless the service holds it (`TitleFile`) |
| `user/<xuid>/` | what the title uploads: the player's multiplayer rank and stats (`mpdata`), starting at level 1 |

Enumerations of Live title servers and downloadable content end at once, empty.

When a Live-signed-in player joins a system-link game, the title asks which
stats to use; the automatic join answers Live (`set useonlinestats 1`), so the
rank is the same in private matches and system link. With `none`, the Live menu
refuses as signed out.

## The next map

Modern Warfare 3's multiplayer only (`MW2_MAP_VOTE=1`, the Android app's "Vote
for the next map after my matches"; `MW2_MAP_ROTATION`). `runtime/map_vote.cpp`.

A console match ends with everyone dropped ("match ended") and the host
starting the next one from its menus; in System Link the others then have to
find and join it again. With the vote on, a System Link or private match this
copy hosts goes straight into another map with every player and bot still
connected:

1. The scripts set the dvar `scr_gameended` when the match is over (2 while a
   round's result is up and another round follows, which is not the end). The
   host looks for it twice a second and prints three choices -- a map and a
   game type each -- as four lines of the message feed, under the winner's
   banner. The names are the menus' own string references, so every player
   reads them in his language, and each line starts with a button drawn from
   the client's own bindings: tactical grenade, use, lethal grenade (LB, X and
   RB on a pad).
2. A press of one of the three is a vote, the last one counting. The host
   reads them from the usercmds the server already has; a frozen player's
   still carry his buttons, though not his sticks. Bots do not vote.
3. The vote closes when every player has answered, when the script calls
   `exitLevel`, or after `MW2_MAP_VOTE_SECONDS` (20). Chance settles a tie or
   no answer. The killcam and the scoreboard draw no feed, so the choices are
   seen for the first seconds only.
4. The script's `exitLevel` is left out, and what the menu's Start Game queues
   is queued for the chosen map. The title's `SV_SpawnServer` tells the
   clients to load it; the runtime carries the server's clients over the
   load, which on the console frees them (`runtime/title.h`, "The
   end-of-match vote").

The maps offered are the ones the menus list whose fastfile is in the game's
folder (or the entries of `MW2_MAP_ROTATION`); the first choice keeps the game
type just played, the others draw one of the game types the menus list
(`MW2_MAP_VOTE_MODES` narrows them). A new game type starts with its own
default rules. `MW2_MAP_ROTATION` without the vote goes through its maps in
order and prints only "Next map".

End Game in the host's pause menu is meant to end the match as it always did
(the menu responses `endround` and `endgame` are watched for).

Run on the PC build: a System Link match with bots ended by its time limit,
the vote answered by the host and by a second copy that had joined through a
relay, and both loading the chosen map and game type with the bots (Seatown
Team Deathmatch to Outpost Capture the Flag, to Village Search and Destroy,
to Dome Domination by rotation). On an AYN Thor a System Link match with four bots
went from Seatown to Lockdown by the vote's draw, nobody voting. Not run: a
private match, End Game.

## Bots and unlocks

Bots are in Modern Warfare 3's multiplayer and in Modern Warfare 2's disc
version (not its title update 6); the unlocks are Modern Warfare 3's only.

**Bots** (`runtime/bots.cpp`, `MW2_BOTS`) join the matches this copy hosts,
a System Link game it created or a private match, and nothing else: in a
match somebody else hosts they do not exist. Where to set them in the Android
app:

| where | what |
|---|---|
| the launcher, **Bots** under **Multiplayer** | how many (0 to 17), how good, and whether all of them play against the players; the entry shows what is set (`BOTS · 6 · REGULAR`) |
| Settings → **Multiplayer** (a tab of its own; it was the end of the Graphics page, below Sound, which is why it was not found) | the same under the heading Bots, then, in Modern Warfare 3, Unlocks, Matches (map vote) and Match options |
| in a match, the app's pause menu → **Bots in my matches** | the number, at once: it sends the host's console `bots <n>` and keeps it as the setting |

The launcher's and the Settings' take effect the next time the game starts.
On the PC they are `MW2_BOTS`, `MW2_BOT_SKILL` and `MW2_BOT_TEAMS` in `.env`.

**Unlocks** (`runtime/mw3_unlock_all.cpp`, `MW2_UNLOCK_ALL`, on by default in
the app) are this copy's reading of its own stats: every item, title and
emblem can be chosen and fifteen custom classes are listed, and nothing is
written to the stats. What other players see, and what a host without the
switch gives, is in
[MW3-MP-BRINGUP.md](MW3-MP-BRINGUP.md#what-other-players-see).

### Modern Warfare 2's bots

The same file plays them: the think (targets, aim, bursts, the feelers), the
top-up, the leave rule, the settings and the console line are shared, and
what differs is the title's block in `runtime/title.h` and three places in
`bots.cpp`.

- **Joining.** The disc's multiplayer still has all of IW's test clients:
  `SV_AddTestClient` (`sub_822BD470`, found by its format `connect bot%d
  "..."`; the script's `addTestClient` calls it) and the flag it sets on the
  client. `bots.cpp` makes that function's calls with a name and a slot of
  its own: the same connect line into the server's argv, `SV_DirectConnect`
  with an address of type 0, the flag, `SV_SendClientGameState` and
  `SV_ClientEnterWorld`. With the flag the server itself spares the bot the
  timeout, the stats a client must send and the pile-up of unacknowledged
  commands, which Modern Warfare 3's bots are given by hand every frame.
- **Team and class.** The menus are `team_marinesopfor` and `changeclass`
  whatever the factions; the answers are `autoassign` (or the side the
  players are not on) and `class0` to `class4`, the five default classes.
- **The usercmd** is 0x28 bytes where Modern Warfare 3's is 0x2C: the weapon
  is a half word and the movement two bytes earlier. The buttons are the
  same.
- **Ammunition.** A bot has one weapon as far as the think knows, so one
  that has nothing left outside the clip is given the full load again with
  the calls of the script's `giveMaxAmmo`. Before that, a match on Terminal
  stopped scoring after three minutes with every bot facing an enemy.
- **Sight.** Both titles' `SV_SightTrace` takes two entities to pass; the
  script's thunk in Modern Warfare 2 passes one, so `bots.cpp` calls the
  function itself in both.

How each address was found is beside it in `runtime/title.h` ("Bots", in the
block of the multiplayer's disc version): the strings and dvars the functions
use, the field tables the scripts read a client and a player state by, and
the instructions that load the server's globals.

Run on the PC build, six bots, Team Deathmatch from System Link, about six
minutes each on Terminal and Rust: every bot joined, chose a class and
spawned once (none was dropped; no "Server command overflow"), the
scoreboards showed their kills and deaths (Rust 1300 to 1800 with 29 kills
between them, Terminal 1000 to 1200 with 22), and the frame rate with six
bots was that with none (Rust, 130 s each: 4193 frames against 4183). The
console lines `bots 2`, `bots +` and `bots skill 0` and `MW2_BOT_TEAMS=against`
were run on Afghan. Those two matches were played before the ammunition was
handed out; with it, Terminal's bots were still scoring in the fifth minute.

Run on an AYN Thor (the app built with `-Dev -Suffix .dev`, six bots set from
the launcher's Bots entry, Terminal from System Link): the bots joined,
spawned and fought, 56 targets went down in four and a half minutes, no bot
was dropped, and the app's counter read 119 to 120 FPS on the 120 Hz screen
throughout.

The leave rule, on the PC: a host with seventeen bots (a full server) and a
second copy on the same network (`MW2_NETWORK=lan`). The second copy found the
game in System Link's list as 18 of 18, joined, and the host logged `Bot Rook
leaves slot 17 (a player needs the slot)` as its connect arrived; the player
chose a team and a class with the bots present, spawned, and was on the
scoreboard beside the host and sixteen bots. Not run: the other path (a party
member whose slot a bot sits in), a Retroid Pocket Flip 2, and a second
device joining an Android host.

Limits, as Modern Warfare 3's: no path data, so a bot feels its way and can
circle on the wrong floor (Terminal's bots met less often than Rust's); no
aiming down the sight, grenades, killstreaks or objectives; the bots are not
in the party, and are added again after a map load. Title update 6 has no
bots: its block has none of these addresses.

## Voice chat

Modern Warfare 3's multiplayer only, and off by default (`MW2_VOICE=1`, the
Android app's Settings > Network > Voice chat). `runtime/kernel/voice.cpp`.

The title's voice library (XHV) is part of the executable and runs as
recompiled code: the voice activity detector, the speech codec and the framing
are its own, and voice travels as the tail of the title's ordinary datagrams,
so lan and the relay carry it unchanged. The runtime supplies what the console's
XAM did, a headset:

| import | behaviour |
|---|---|
| `XamVoiceCreate` | voice on and the first player: opens the microphone and writes a handle. Otherwise succeeds with no handle, which XHV reads as no headset |
| `XamVoiceHeadsetPresent` | 1 while that headset is open |
| `XamVoiceSubmitPacket`, direction 1 | capture: the packet is queued and filled with 16 kHz 16-bit big-endian samples as the microphone delivers them, four times the bytes asked for with format 0, which XHV takes as already decoded |
| `XamVoiceSubmitPacket`, direction 0 | playback: 4-bit ADPCM at 8 kHz, 64 bytes every 16 ms, decoded by calling the title's own coder (`T_XHV_AdpcmCoder` in `runtime/title.h`), resampled to 48 kHz and added to the front channels of the mix (`runtime/apu/audio.cpp`) |
| `XamVoiceClose` | drops the queued packets and closes the microphone |

The microphone is SDL's default recording device on the desktop and an AAudio
input stream with the voice-communication preset on Android
(`runtime/android/voice.cpp`). Without one the other players are still heard.

**Privacy.** The console's title has no push-to-talk: it sends whenever it
hears speech. Here nothing is sent unless voice chat is on, and then only
while push-to-talk is held: the T key on the desktop, the Talk button of the
touch controls on Android (`MW2_VOICE_PTT=0` for an open microphone). The
Android app asks for the microphone permission when the player turns voice
chat on, and at no other time; refused, voice chat stays off and the game is
unaffected. Speech is not encrypted, as it was not on the console: with the
`server` service it passes through the relay, whose operator could record it.
Nothing is written to disk except by `MW2_VOICE_DUMP`.

**Who is heard.** The title decides, as on the console:

- Only a player signed in to Xbox LIVE has a headset: the title asks for the
  communications privilege and registers no talker without it. A relay
  (`MW2_NETWORK=server`) signs in; `lan` does not unless `MW2_LIVE=1`, and the
  log then says so once (`voice: the title keeps voice chat for players signed
  in to Xbox LIVE`).
- In a match the title sends a player's voice only to those its rules let
  hear him (`sub_821659C8`: team, alive or dead, and the dead-chat dvars), so
  two players on opposite teams of a Team Deathmatch hear nothing of each
  other. Players who have not chosen a team yet hear each other.
- XHV scales what is heard by the profile's voice volume
  (`XPROFILE_OPTION_VOICE_VOLUME`, `0x1004000E`); a profile that never stored
  one reads as 100 (`runtime/kernel/profile.cpp`), as at 0 nobody is heard.

**Testing with two copies.** On copy A set `MW2_VOICE=1`, `MW2_VOICE_TONE=1`
and `MW2_VOICE_PTT=0`; on copy B `MW2_VOICE=1` and `MW2_VOICE_DUMP=b.wav`,
both through a relay. A hosts a System Link game and B joins it and stays at
CHOOSE TEAM: A's tone is in `b.wav` (8 kHz; the codec is a speech codec, so
the tone comes back as a narrow band around 440 Hz, not a line). Each copy
logs its first three packets in each direction (`voice: capture packet ...`,
`voice: playback packet ...`) with their size and format. With
`MW2_NO_AUDIO=1` on Windows the mixer runs at about 0.7 of real time, and
the earpiece, which is fed from the mix, with it.

Run that way on two Windows copies through a local relay. Not yet run: a
real microphone, the Android capture path, push-to-talk, the talker icon and
the title's scoreboard mute.

## Switches

| switch | effect |
|---|---|
| `TITLE=sp\|mp` | `build.sh`: which executable to build |
| `MW2_NETWORK=none\|lan\|server\|steam` | which online service; the launcher's NETWORK, the Android app's Settings. MW3's multiplayer: none, lan, server |
| `MW2_SERVER=<host>[:<port>]` | server: the relay's address |
| `MW2_SERVER_PASSWORD` | server: the relay's password |
| `MW2_NET_LINK=0\|1` | Ethernet link reported down or up (default: up in MW2's multiplayer, up in MW3's when a service started, down in the campaigns) |
| `MW2_LIVE=1` | MW3's multiplayer: signed in to Xbox LIVE (experimental; MW2's is whenever the service carries lobbies) |
| `MW2_NAME` | lan and server: the player's name |
| `MW2_LAN_PORT` | lan: the shared broadcast port (3074) |
| `MW2_LAN_JOIN=<name>` | lan: join that player's party or lobby when seen |
| `MW2_LAN_ACCEPT=1` | lan: accept invitations |
| `MW2_STEAM_ROOT=<path>` | steam: the install to use, the folder holding `linux64/steamclient.so`, or `steamclient64.dll` on Windows; on Linux a Steam must be running from it |
| `MW2_STEAM_APPID` | steam: the app id to run as (480) |
| `MW2_VOICE=1` | MW3's multiplayer: voice chat (off by default; see [Voice chat](#voice-chat)) |
| `MW2_VOICE_PTT=0` | voice: an open microphone instead of push-to-talk |
| `MW2_TRACE_ONLINE=1` | log every datagram (diagnostic builds) |

The full list is in [switches.md](switches.md).
