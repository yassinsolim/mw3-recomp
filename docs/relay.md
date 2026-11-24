# The relay server

A relay is what players choose with NETWORK SERVER in the launcher. Everyone
connected to the same relay sees each other's System Link games, can join
each other's private matches, and finds games with PLAY ONLINE's Find Game,
wherever they are. The relay plays no match itself: whoever hosts a match plays
in it, as on the console, and a host who ends the game hands the match to another
player ([multiplayer.md](multiplayer.md#host-migration)).

Every packet goes through the relay. Players connect out to it, so only the
relay's machine needs an open port, and no player learns another's address.

A relay serves any title built from this source -- Modern Warfare 2, Modern
Warfare 3's multiplayer -- and keeps them apart: a copy meets only copies of
its own title and network version, as if each had a relay to itself.

## Running one

The relay is one small program, `mw2-relay`, for Linux. It listens on UDP port
3074 unless told otherwise. Its version must be the players' release: a relay
and a game of different protocols refuse each other, and the game's log says
so.

A relay is **private**, for those who have its password, or **public**, for
anyone who has its address: `--public`, which is also what a relay with no
password is. `--public` with a password, from `--password` or the environment,
is a mistake the relay does not start with. A public relay reads no password,
so a copy with one set still joins.

**With Docker**, from the image each release publishes:

    docker run -d --restart unless-stopped -p 3074:3074/udp \
      -e MW2_RELAY_PASSWORD=<password> ghcr.io/paulcombal/mw2-relay:<release>

The `/udp` matters: without it Docker forwards TCP, which the relay does not
use. The image runs as an unprivileged user and holds the binary alone.

With Compose:

    services:
      mw2-relay:
        image: ghcr.io/paulcombal/mw2-relay:<release>
        restart: unless-stopped
        ports: ["3074:3074/udp"]
        environment:
          MW2_RELAY_PASSWORD: <password>

To build the image yourself: `docker build -t mw2-relay relay/`.

**Without Docker**, take `mw2-relay-<release>-linux-x86_64` from the release
(a static binary that needs no library), and run it:

    ./mw2-relay --port 3074 (--public | --password <password>) [--max 64]
                [--per-address 16] [--sessions-per-address 4] [--rate 1000]
                [--total-rate 20000] [--max-payload 2048]
                [--playlists [<title>=]<file>]... [--log-file <file>]
                [--status-port <port>] [--status-address 127.0.0.1]

`--max` is how many players it takes at once, `--per-address` how many of them
from one IP address: players behind one router share theirs, so a LAN party
on a relay needs as many as it has players. The password can come from the
environment instead (`MW2_RELAY_PASSWORD`), which keeps it out of the process
list. Built from source, `cargo build --release` in `relay/`. The other limits
are [below](#limits).

**A public relay on a plain Linux machine or container**, with systemd. Put
the binary in `/usr/local/bin`, and this in
`/etc/systemd/system/mw2-relay.service`:

    [Unit]
    Description=mw2-relay, public
    After=network-online.target
    Wants=network-online.target

    [Service]
    ExecStart=/usr/local/bin/mw2-relay --public --port 3074 --max 256 \
        --per-address 8 --status-port 3075
    DynamicUser=yes
    Restart=always
    RestartSec=2
    NoNewPrivileges=yes
    ProtectSystem=strict
    ProtectHome=yes
    PrivateTmp=yes
    PrivateDevices=yes
    RestrictAddressFamilies=AF_INET
    CapabilityBoundingSet=
    MemoryMax=128M
    TasksMax=16

    [Install]
    WantedBy=multi-user.target

then `systemctl daemon-reload && systemctl enable --now mw2-relay`. The log is
the journal's (`journalctl -u mw2-relay -f`), which keeps its own size, and
`curl 127.0.0.1:3075` says what the relay holds. It needs no root, no file and
no other port. In an unprivileged LXC container without nesting, systemd
cannot make the namespaces the `Protect*` and `Private*` lines ask for and the
unit fails with `226/NAMESPACE`: turn nesting on for the container, or remove
those lines and `DynamicUser` and write `User=nobody` instead. Forward UDP 3074
from the router to the container; the players' SERVER is the router's public
name and that port.

`--playlists` is the file Find Game offers (below); the image starts with
Modern Warfare 2's. A title the relay has no file for is offered the playlists
each copy was built with, the same in every copy of a release.
`<title>=` before the file, the title's id in hexadecimal, makes it that
title's alone, and the option can be given once for each: `--playlists
41560817=mw2.info --playlists 415608cb=mw3.info`. `--log-file` also writes
the log to a file, for a Docker that shows no container output: mount a folder
that user 65534 can write to, and name a file in it. `--status-port` answers
any TCP connection with what the relay holds, as JSON: who is connected and as
which title, the sessions listed and their numbers, the latest searches with
how many sessions each number turned away, and how many packets each limit
dropped. It has no password and shows the players' addresses, so it listens
on 127.0.0.1 alone unless `--status-address` says otherwise. In a container
it must, for Docker to publish it, on the machine alone:

    docker run -d -p 3074:3074/udp -p 127.0.0.1:3075:3075/tcp -v /srv/relay:/log \
      ghcr.io/paulcombal/mw2-relay:<release> --public \
      --log-file /log/relay.log --status-port 3075 --status-address 0.0.0.0
    curl 127.0.0.1:3075

**The firewall** lets in UDP on the relay's port, from anywhere. Players need
nothing open.

**The players** set, under NETWORK in the launcher: NETWORK SERVER, SERVER to
the relay's address (`relay.example.com`, or `relay.example.com:<port>` for
another port than 3074), and PASSWORD.

The relay logs a line, with the time in UTC, for each player who joins,
leaves, goes quiet, or is refused, and why, and for each packet a limit
drops: at most 20 lines every 10 s about each of these, then one line that
counts the rest, so a flood does not fill the disk.

## Limits

A relay anyone can reach is sent anything. What it takes is bounded, and so
is what it holds:

| limit | default | why |
|---|---|---|
| players at once | `--max` 64 | the copies table, and every table that follows it |
| players from one IP address | `--per-address` 16 | a household or a LAN party shares one; one machine cannot fill the relay |
| sessions listed from one IP address | `--sessions-per-address` 4 | a copy lists one; one address cannot fill Find Game |
| packets a second from one player | `--rate` 1000, twice that at once | a host of 18 sends some 350, a player 30 to 100; an All counts once for each player it goes to |
| packets a second from all players | `--total-rate` 20000 | what a small machine and a home uplink carry; 64 players in matches send a fifth of it |
| bytes after a packet's header | `--max-payload` 2048 | the titles send 1264 at most, wrapped in 12; with the rate, it bounds a player's bandwidth |
| listings a second from one player | 4, 8 at once | the game lists every 2 s and on a change |
| searches | one a player every 0.9 s, 200 a second in all | a search walks every listing |
| attributes of a listing, wants of a search | 32 | a listing is 340 bytes at most |
| results of a search | 50, in pages of 12 | pages stay under 1200 bytes |
| Hellos a second from one IP address that has not joined | 20, 40 at once | two join a copy; 16 copies rejoin at once after a restart |
| other packets a second from such an address | 5, 10 at once | each is answered Unknown |
| answers a second to all such addresses | 500, 1000 at once | a flood of forged addresses gets no more Challenges than that |
| such addresses told apart | 4096, each for 10 s | past it the line above alone holds |
| playlists | 8 files of 128 KB | one a title |

A packet from an address that has not joined, or without its copy's token,
counts against that address and never against a copy, so nobody slows a
player down by sending in its name. What a limit drops gets no answer.

## What it costs

The relay forwards the game's traffic, a few kilobytes a second per player in
a match, so a small virtual server carries several matches. Each packet
travels player, relay, player: a relay close to its players keeps the lag low.

## Playlists

The file is the title's own format. It lists the game types, then the
playlists numbered from 0 with no gap, of at most 24, and is at most 128 KB,
which is what the title has room for. Every copy on a relay fetches it as it
connects, so all have the same bytes; the title compares `version` when a player
joins a party, so raise it when the file changes.

    version 1

    gametype tdm                       // any name, used below
    name english "Team Deathmatch"     // quoted; a word alone stops at the space
    script war                         // the script game type
    teambased

    playlist 0
    name english "Team Deathmatch"
    description english "Two teams, most kills wins."
    minparty 1                         // smallest group of friends it accepts
    maxparty 9                         // largest
    rule party_minplayers 2            // a dvar, set when the playlist is
    mp_afghan,tdm,1                    // map, game type, weight: no spaces

A flag `ranked` on a playlist makes it arbitrated, which nothing here answers.
Advanced Warfare's file also has categories, which say which of its menus
lists a playlist
([AW-BRINGUP.md](AW-BRINGUP.md#exo-survival-under-xbox-live-combat-readiness-program-24-november-2026-windows)).
`rule party_hostmigration 0` keeps a lobby's first host, so a match ends when it
leaves ([host migration](multiplayer.md#host-migration)).

An error in the file does not stop anything: the title drops the line, or the
playlist. A diagnostic build logs why with `MW2_ENGINE_LOG=1`.

## What players on a relay should know

On a private relay, anyone with the password sees every game of their title
hosted on the relay and can join it, and meets every other player who looks
for a game with Find Game; keep the password to the people the relay is for.
On a public relay that is anyone at all. The relay's owner can see all the
traffic and every player's address; the players see neither. What a player has
unlocked or wears is its own copy's and never passes the relay's judgement:
the relay hands the game's packets on unread.

## How it works

The relay is a Rust program of its own in `relay/`. It uses nothing of the
game's: all they share is the protocol, `relay/src/protocol.rs`, from which
`relay/protocol.h` is made for the game's side, `runtime/online/server.cpp`
([multiplayer.md](multiplayer.md#lan-and-server)). `cargo test` fails when the
two differ, and `BLESS=1 cargo test` writes the header again. A number in a packet
has a big-endian type, so it cannot be read without saying so, and nothing in
the program parses a packet by hand.

`cargo test` also runs the real program on a real socket, as a copy speaks to
it (`relay/tests/conformance.rs`), with packets of random bytes among them.

Every packet is a 24-byte header -- the magic `MW2R`, a kind, a peer id and
a token -- then what the kind carries, big-endian, 2048 bytes at most:

| kind | from | carries |
|---|---|---|
| Hello | game | the protocol, the title's id and its network version, the password; the id the copy had, or 0; the Challenge's token, or 0 |
| Challenge | relay | the token every later packet of this copy carries |
| Welcome | relay | the copy's id |
| Refused | relay | why, as text: wrong password, another protocol, full |
| Alive | game | nothing; sent when the copy has said nothing for a second |
| To | game | a peer's id and the bytes for it |
| All | game | bytes for everyone else of its title and version on the relay |
| From | relay | the sender's id, which the relay stamps, and the bytes |
| Unknown | relay | nothing: the relay does not know this copy, which says Hello again |
| Bye | game | nothing: the copy is leaving |
| Advertise | game | a listing and its numbers, or that it is withdrawn |
| Search | game | the room and the numbers wanted |
| Results | relay | a page of what fits, nearest host first |
| Ping | game | anything in `peer` |
| Pong | relay | the Ping's `peer` |
| FileAsk | game | an offset in its title's playlists |
| FileChunk | relay | the file's size, the offset, and up to 1 KB of it |

UDP lets anyone write any sender address on a packet, so the relay keeps
nothing for a Hello until the address has shown it receives what is sent to
it: the first Hello gets a Challenge with a token, SipHash-2-4 of the address
and port under a key the relay draws at each start, and only a Hello carrying
that token is taken in. Every later packet must carry it too; one that does
not is dropped, so nobody can send, leave, or take a slot as another address.
To an address it has not checked, the relay sends nothing larger than what it
got from there, so it cannot be used to multiply traffic at someone else. A
relay restarted with a new key answers the old tokens with Unknown, and the
copies are challenged again. A public relay challenges all the same.

**Titles.** A Hello names the copy's title, the id of its executable
(`41560817` Modern Warfare 2, `415608CB` Modern Warfare 3's multiplayer), and
the title's network version. The relay compares the two numbers and knows no
more of them: To reaches only a copy with the same pair, All and Search see
only those, and a session is 60 bytes and some numbers whatever the title. A
Hello of another protocol is refused with the reason before the rest of it is
read, so a game of an older release is told why.

A copy is known by the address and port its packets come from. The relay keeps
only the copies it took in. The relay
forgets one it has not heard from for ten seconds; the game's Alive keeps a
copy known while it loads a map. A copy that says Hello again asks for the id
it had, and gets it back unless someone else has it, so after a restart of
the relay every copy is back, under its own id, within a second or so. Ids are 48 bits, because the game makes
the Ethernet address the title tells machines apart by from them.

**Matchmaking** is `relay/src/directory.rs`, which has no sockets. A copy lists one
session at most: 60 bytes the relay hands on, the slots, and the numbers a search
compares, which mean nothing to the relay. The copy says it again every 2 s, so
a lost packet or a restarted relay costs nothing, and a sequence number keeps a
late packet from undoing a newer one. A listing goes with its copy. Search
compares each number it wants, equal or at least, and answers in pages of 12,
which stay under 1200 bytes so that the Internet does not split them, and always
answers, since a search with no answer costs the asker 3 s. A copy gets one
search a second. The relay stamps each listing with the id of the copy that sent
it, and the game builds the host's address from that id, so nobody can list a
session in another player's name.

Each copy measures its round trip to the relay with Ping, keeps the shortest of
the last eight, and lists it. Every packet of a match goes through the relay, so
the round trip between two players is the sum of their two, and that is what the
title's QoS is told.

`PROTOCOL` is raised whenever a packet's meaning changes.
