//! Who is connected, and what each packet does.

use crate::directory::{Directory, Listing, Realm, Tally, Wanted};
use crate::limits::{About, Bucket, Quiet, Rate};
use crate::log::log;
use crate::protocol::*;
use serde_json::{Map, Value, json};
use siphasher::sip::SipHasher24;
use std::collections::{BTreeMap, VecDeque};
use std::hash::Hasher;
use std::net::{Ipv4Addr, SocketAddrV4, UdpSocket};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use zerocopy::{FromBytes, IntoBytes};

/// A search walks every listing: once a second is more than the title asks for.
const SEARCH_EVERY: Duration = Duration::from_millis(900);
/// All the searches of a relay, whoever asks.
const SEARCHES: Rate = Rate::of(200);
/// A copy says its listing every ADVERTISE_EVERY_SECONDS, and when it changes.
const ADVERTS: Rate = Rate::of(4);
const ASKED_KEPT: usize = 32;

/// What an IP address with no copy here may send: the two Hellos that join
/// each of its copies, and little else, which Unknown answers.
const HELLOS: Rate = Rate::of(20);
const OTHERS: Rate = Rate::of(5);
/// What all such addresses together are answered: a flood of forged sender
/// addresses gets this many answers a second and no more.
const ALL_STRANGERS: Rate = Rate::of(500);
/// How many such addresses are told apart. Past it, ALL_STRANGERS alone holds.
const STRANGERS_KEPT: usize = 4096;
const STRANGER_KEPT_FOR: Duration = Duration::from_secs(10);

struct GameCopy {
    address: SocketAddrV4,
    id: u64,
    token: u64,
    realm: Realm,
    heard: Instant,
    searched: Option<Instant>,
    packets: Bucket,
    adverts: Bucket,
}

/// An IP address that sent something and has no copy here, or sent it without
/// a copy's token.
struct Stranger {
    hellos: Bucket,
    others: Bucket,
    seen: Instant,
}

/// One search, as the status page shows it.
struct Asked {
    when: u64,
    asker: u64,
    room: u32,
    wants: Vec<Wanted>,
    tally: Tally,
    found: usize,
}

pub struct Limits {
    /// empty: anyone joins, whatever password it gives
    pub password: String,
    pub most: usize,
    pub per_address: usize,
    /// listings from one IP address; each copy has one at most
    pub sessions_per_address: usize,
    /// bytes after the Header
    pub payload: usize,
    /// what one copy sends; an All counts once for each copy it goes to
    pub copy: Rate,
    /// what all copies send together
    pub all: Rate,
}

/// The file Find Game offers, for each title that has one.
#[derive(Default)]
pub struct Playlists {
    files: Vec<(Option<u32>, Vec<u8>)>,
}

impl Playlists {
    /// False when this title, or with None every title without its own,
    /// already has a file.
    pub fn add(&mut self, title: Option<u32>, file: Vec<u8>) -> bool {
        if self.files.iter().any(|(held, _)| *held == title) {
            return false;
        }
        self.files.push((title, file));
        true
    }

    pub fn count(&self) -> usize {
        self.files.len()
    }

    fn of(&self, title: u32) -> &[u8] {
        let named = self.files.iter().find(|(held, _)| *held == Some(title));
        named.or_else(|| self.files.iter().find(|(held, _)| held.is_none())).map_or(&[], |(_, file)| file)
    }
}

/// Packets dropped since the start, by why.
#[derive(Default)]
struct Dropped {
    too_large: u64,
    strangers: u64,
    copy_rate: u64,
    all_rate: u64,
    adverts: u64,
    searches: u64,
}

pub struct Relay {
    socket: UdpSocket,
    limits: Limits,
    keys: (u64, u64), // make the tokens; new at every start
    // Every table holds a fixed number of entries at most: copies and ids
    // --max, the directory one listing a copy, strangers STRANGERS_KEPT,
    // asked ASKED_KEPT.
    copies: BTreeMap<SocketAddrV4, GameCopy>,
    ids: BTreeMap<u64, SocketAddrV4>,
    strangers: BTreeMap<Ipv4Addr, Stranger>,
    all_strangers: Bucket,
    all: Bucket,
    searches: Bucket,
    playlists: Playlists,
    directory: Directory,
    asked: VecDeque<Asked>, // the newest searches, for the status page
    dropped: Dropped,
    quiet: Quiet,
    packet: Vec<u8>,
}

/// A number from the system's own source of randomness. Failing is failing to
/// start: tokens from a guessable key protect nothing.
fn random() -> u64 {
    let mut bytes = [0; 8];
    getrandom::fill(&mut bytes).expect("the system gives no random numbers");
    u64::from_ne_bytes(bytes)
}

fn id_text(id: u64) -> String {
    format!("{id:012x}")
}

fn hexadecimal(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn number(id: u32) -> String {
    format!("0x{id:08x}")
}

fn realm_text(realm: Realm) -> String {
    format!("title {:08x} version {}", realm.title, realm.version)
}

fn seconds_now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |since| since.as_secs())
}

impl Relay {
    pub fn new(socket: UdpSocket, limits: Limits, playlists: Playlists) -> Relay {
        let now = Instant::now();
        Relay {
            socket,
            keys: (random(), random()),
            copies: BTreeMap::new(),
            ids: BTreeMap::new(),
            strangers: BTreeMap::new(),
            all_strangers: Bucket::full(ALL_STRANGERS, now),
            all: Bucket::full(limits.all, now),
            searches: Bucket::full(SEARCHES, now),
            limits,
            playlists,
            directory: Directory::default(),
            asked: VecDeque::new(),
            dropped: Dropped::default(),
            quiet: Quiet::new(now),
            packet: Vec::new(),
        }
    }

    pub fn heard(&mut self, packet: &[u8], from: SocketAddrV4) {
        let Ok((header, body)) = Header::read_from_prefix(packet) else { return };
        if header.magic.get() != MAGIC {
            return;
        }
        let now = Instant::now();
        if body.len() > self.limits.payload {
            self.dropped.too_large += 1;
            if self.quiet.lets(About::Limits, now) {
                log!("dropped a packet from {from}: it carries {} bytes, above {}", body.len(), self.limits.payload);
            }
            return;
        }
        let kind = Kind::from_wire(header.kind);
        let token = header.token.get();
        let hello = kind == Some(Kind::Hello);
        // A packet without its copy's token is somebody else's, sending as
        // the copy's address: it costs the copy nothing.
        let Some(copy) = self.copies.get_mut(&from).filter(|copy| copy.token == token) else {
            if !self.stranger(*from.ip(), hello, now) {
                return;
            }
            if hello {
                self.hello(from, header.peer.get() & ID_MASK, token, body);
            } else if kind != Some(Kind::Bye) && !self.copies.contains_key(&from) {
                // A copy this relay forgot, or never knew: it says Hello again.
                self.say(from, Kind::Unknown, 0, &[], 0);
            }
            return;
        };
        copy.heard = now;
        let (id, realm) = (copy.id, copy.realm);
        // An All is as many packets as it has copies to go to.
        let mut cost = 1.0;
        if kind == Some(Kind::All) {
            let others = self.copies.values().filter(|other| other.realm == realm).count() - 1;
            cost = (others as f64).clamp(1.0, self.limits.copy.a_second);
        }
        let copy = self.copies.get_mut(&from).expect("found above");
        if !copy.packets.take(self.limits.copy, cost, now) {
            self.dropped.copy_rate += 1;
            if self.quiet.lets(About::Limits, now) {
                log!(
                    "dropped a packet of {} ({from}): more than {} a second",
                    id_text(id),
                    self.limits.copy.a_second
                );
            }
            return;
        }
        if !self.all.take(self.limits.all, cost, now) {
            self.dropped.all_rate += 1;
            if self.quiet.lets(About::Limits, now) {
                log!("dropped a packet: more than {} a second from all copies", self.limits.all.a_second);
            }
            return;
        }
        match kind {
            Some(Kind::Hello) => self.hello(from, id, token, body),
            Some(Kind::To) => {
                let to = self.ids.get(&header.peer.get()).and_then(|to| self.copies.get(to));
                if let Some(to) = to.filter(|to| to.realm == realm).map(|to| to.address) {
                    self.say(to, Kind::From, id, body, 0);
                }
            }
            Some(Kind::All) => {
                let others: Vec<SocketAddrV4> = self
                    .copies
                    .values()
                    .filter(|other| other.id != id && other.realm == realm)
                    .map(|other| other.address)
                    .collect();
                for to in others {
                    self.say(to, Kind::From, id, body, 0);
                }
            }
            Some(Kind::Advertise) => self.advertise(from, id, realm, body),
            Some(Kind::Search) => self.search(from, id, realm, body),
            Some(Kind::Ping) => self.say(from, Kind::Pong, header.peer.get(), &[], 0),
            Some(Kind::FileAsk) => self.send_chunk(from, realm.title, body),
            Some(Kind::Bye) => {
                if self.quiet.lets(About::Joining, now) {
                    log!("{} left ({from}), {} here", id_text(id), self.copies.len() - 1);
                }
                self.forget(from);
            }
            _ => {}
        }
    }

    /// True when a packet from an address with no copy here may be answered.
    fn stranger(&mut self, ip: Ipv4Addr, hello: bool, now: Instant) -> bool {
        if !self.strangers.contains_key(&ip) && self.strangers.len() < STRANGERS_KEPT {
            let new = Stranger { hellos: Bucket::full(HELLOS, now), others: Bucket::full(OTHERS, now), seen: now };
            self.strangers.insert(ip, new);
        }
        let fits = self.strangers.get_mut(&ip).is_none_or(|stranger| {
            stranger.seen = now;
            if hello { stranger.hellos.take(HELLOS, 1.0, now) } else { stranger.others.take(OTHERS, 1.0, now) }
        });
        if fits && self.all_strangers.take(ALL_STRANGERS, 1.0, now) {
            return true;
        }
        self.dropped.strangers += 1;
        if self.quiet.lets(About::Limits, now) {
            log!("dropped a packet from {ip}: too many from addresses that have not joined");
        }
        false
    }

    /// Every copy not heard from for FORGET_AFTER_SECONDS: its game ended
    /// without a Bye, or its connection did.
    pub fn sweep(&mut self) {
        let now = Instant::now();
        let after = Duration::from_secs(FORGET_AFTER_SECONDS.into());
        let quiet: Vec<SocketAddrV4> = self
            .copies
            .values()
            .filter(|copy| now.duration_since(copy.heard) >= after)
            .map(|copy| copy.address)
            .collect();
        for address in quiet {
            if let Some(copy) = self.copies.get(&address)
                && self.quiet.lets(About::Joining, now)
            {
                log!("{} went quiet ({address}), {} here", id_text(copy.id), self.copies.len() - 1);
            }
            self.forget(address);
        }
        self.strangers.retain(|_, stranger| now.duration_since(stranger.seen) < STRANGER_KEPT_FOR);
        self.quiet.sweep(now);
    }

    /// What it holds right now, as JSON.
    pub fn status(&self) -> String {
        let copies: Vec<Value> = self
            .copies
            .values()
            .map(|copy| {
                json!({
                    "id": id_text(copy.id),
                    "address": copy.address.to_string(),
                    "title": format!("{:08x}", copy.realm.title),
                    "version": copy.realm.version,
                })
            })
            .collect();
        let sessions: Vec<Value> = self
            .directory
            .listings()
            .iter()
            .map(|(host, listing)| {
                let attributes: Map<String, Value> =
                    listing.attributes.iter().map(|&(id, value)| (number(id), value.into())).collect();
                json!({
                    "host": id_text(*host),
                    "title": format!("{:08x}", listing.realm.title),
                    "version": listing.realm.version,
                    "session": hexadecimal(&listing.session[..8]),
                    "rtt": listing.rtt,
                    "openPublic": listing.open_public,
                    "openPrivate": listing.open_private,
                    "filledPublic": listing.filled_public,
                    "filledPrivate": listing.filled_private,
                    "attributes": attributes,
                })
            })
            .collect();
        let searches: Vec<Value> = self
            .asked
            .iter()
            .map(|asked| {
                let wants: Map<String, Value> = asked
                    .wants
                    .iter()
                    .map(|want| {
                        let compare = if want.at_least { ">=" } else { "==" };
                        (number(want.id), format!("{compare}{}", want.value).into())
                    })
                    .collect();
                let refused: Map<String, Value> =
                    asked.tally.refused_by.iter().map(|(&id, &count)| (number(id), count.into())).collect();
                json!({
                    "when": asked.when,
                    "asker": id_text(asked.asker),
                    "room": asked.room,
                    "wants": wants,
                    "listed": asked.tally.listed,
                    "full": asked.tally.full,
                    "refusedBy": refused,
                    "found": asked.found,
                })
            })
            .collect();
        let playlists: Vec<Value> = self
            .playlists
            .files
            .iter()
            .map(|(title, file)| json!({ "title": title.map(|title| format!("{title:08x}")), "bytes": file.len() }))
            .collect();
        let status = json!({
            "protocol": PROTOCOL,
            "public": self.limits.password.is_empty(),
            "playlists": playlists,
            "copies": copies,
            "sessions": sessions,
            "searches": searches,
            "dropped": {
                "tooLarge": self.dropped.too_large,
                "strangers": self.dropped.strangers,
                "copyRate": self.dropped.copy_rate,
                "allRate": self.dropped.all_rate,
                "adverts": self.dropped.adverts,
                "searches": self.dropped.searches,
            },
        });
        format!("{status}\n")
    }

    fn token_of(&self, address: SocketAddrV4) -> u64 {
        let word = (u64::from(u32::from(*address.ip())) << 16) | u64::from(address.port());
        let mut hasher = SipHasher24::new_with_keys(self.keys.0, self.keys.1);
        hasher.write_u64(word);
        match hasher.finish() {
            0 => 1, // 0 is no token
            token => token,
        }
    }

    fn hello(&mut self, from: SocketAddrV4, had: u64, token: u64, body: &[u8]) {
        // Nothing is kept and nothing larger than the Hello goes back until
        // the address has shown it receives what is sent to it.
        let Ok((protocol, _)) = Be16::read_from_prefix(body) else { return };
        if body.len() < SHORTEST_HELLO {
            return;
        }
        let expected = self.token_of(from);
        if token != expected {
            self.say(from, Kind::Challenge, 0, &[], expected);
            return;
        }
        // A game of another protocol is told so, whatever its Hello holds.
        let protocol = protocol.get();
        if protocol != PROTOCOL {
            self.refuse(
                from,
                &format!("this relay speaks protocol {PROTOCOL}, the game {protocol}: they come from different releases"),
            );
            return;
        }
        let Ok(hello) = Hello::read_from_bytes(body) else { return };
        let realm = Realm { title: hello.title.get(), version: hello.version.get() };
        let end = hello.password.iter().position(|&byte| byte == 0).unwrap_or(hello.password.len());
        let password = &hello.password[..end];
        // A relay without a password is open to all, whatever they give.
        if !self.limits.password.is_empty() && password != self.limits.password.as_bytes() {
            let why = if password.is_empty() { "a password is needed" } else { "wrong password" };
            self.refuse(from, why);
            return;
        }
        let now = Instant::now();
        if self.copies.get(&from).is_some_and(|known| known.realm != realm) {
            // Another game on the port a copy had.
            self.forget(from);
        }
        if let Some(known) = self.copies.get_mut(&from) {
            // Its Welcome was lost; the same answer again.
            known.heard = now;
            let id = known.id;
            self.say(from, Kind::Welcome, id, &[], 0);
            return;
        }
        if self.copies.len() >= self.limits.most {
            self.refuse(from, "the relay is full");
            return;
        }
        let from_there = self.copies.values().filter(|copy| copy.address.ip() == from.ip()).count();
        if from_there >= self.limits.per_address {
            self.refuse(from, "too many players from this address");
            return;
        }
        // The id it had, if nobody has it now; a new one otherwise.
        let mut id = if self.ids.contains_key(&had) { 0 } else { had };
        while id == 0 || self.ids.contains_key(&id) {
            id = random() & ID_MASK;
        }
        let copy = GameCopy {
            address: from,
            id,
            token: expected,
            realm,
            heard: now,
            searched: None,
            packets: Bucket::full(self.limits.copy, now),
            adverts: Bucket::full(ADVERTS, now),
        };
        self.copies.insert(from, copy);
        self.ids.insert(id, from);
        if self.quiet.lets(About::Joining, now) {
            log!("{} joined ({from}), {}, {} here", id_text(id), realm_text(realm), self.copies.len());
        }
        self.say(from, Kind::Welcome, id, &[], 0);
    }

    fn refuse(&mut self, to: SocketAddrV4, why: &str) {
        if self.quiet.lets(About::Refusals, Instant::now()) {
            log!("refused {to}: {why}");
        }
        self.say(to, Kind::Refused, 0, why.as_bytes(), 0);
    }

    fn forget(&mut self, address: SocketAddrV4) {
        if let Some(copy) = self.copies.remove(&address) {
            self.directory.forget(copy.id);
            self.ids.remove(&copy.id);
        }
    }

    fn advertise(&mut self, from: SocketAddrV4, id: u64, realm: Realm, body: &[u8]) {
        let Ok((advert, rest)) = Advert::read_from_prefix(body) else { return };
        let now = Instant::now();
        let Some(copy) = self.copies.get_mut(&from) else { return };
        if !copy.adverts.take(ADVERTS, 1.0, now) {
            self.dropped.adverts += 1;
            return;
        }
        let sequence = advert.sequence.get();
        if advert.open == 0 {
            if self.directory.withdraw(id, sequence) && self.quiet.lets(About::Listings, now) {
                log!("{} withdrew its session", id_text(id));
            }
            return;
        }
        let count = usize::from(advert.attributes);
        if count > MOST_ATTRIBUTES {
            return;
        }
        let Ok(attributes) = <[Attribute]>::ref_from_bytes_with_elems(rest, count) else { return };
        let known = self.directory.holds(id);
        if !known {
            // A copy lists one session; an address, as many as it may.
            let there = self.copies.values().filter(|copy| copy.address.ip() == from.ip());
            let listed = there.filter(|copy| self.directory.holds(copy.id)).count();
            if listed >= self.limits.sessions_per_address {
                self.dropped.adverts += 1;
                if self.quiet.lets(About::Refusals, now) {
                    log!("refused a session of {} ({from}): {listed} are listed from its address", id_text(id));
                }
                return;
            }
        }
        let listing = Listing {
            realm,
            rtt: advert.rtt.get(),
            session: advert.session,
            open_public: advert.open_public.get(),
            open_private: advert.open_private.get(),
            filled_public: advert.filled_public.get(),
            filled_private: advert.filled_private.get(),
            attributes: attributes.iter().map(|attribute| (attribute.id.get(), attribute.value.get())).collect(),
        };
        let open = listing.open_public;
        if self.directory.put(id, sequence, listing) && !known && self.quiet.lets(About::Listings, now) {
            log!("{} listed session {}, {open} public slots open", id_text(id), hexadecimal(&advert.session[..8]));
        }
    }

    fn search(&mut self, from: SocketAddrV4, id: u64, realm: Realm, body: &[u8]) {
        let Ok((search, rest)) = Search::read_from_prefix(body) else { return };
        let count = usize::from(search.wants.get());
        if count > MOST_ATTRIBUTES {
            return;
        }
        let Ok(wants) = <[Want]>::ref_from_bytes_with_elems(rest, count) else { return };
        let now = Instant::now();
        let Some(copy) = self.copies.get_mut(&from) else { return };
        if copy.searched.is_some_and(|searched| now.duration_since(searched) < SEARCH_EVERY) {
            self.dropped.searches += 1;
            return;
        }
        copy.searched = Some(now);
        if !self.searches.take(SEARCHES, 1.0, now) {
            self.dropped.searches += 1;
            if self.quiet.lets(About::Limits, now) {
                log!("dropped a search: more than {} a second from all copies", SEARCHES.a_second);
            }
            return;
        }

        let wants: Vec<Wanted> = wants
            .iter()
            .map(|want| Wanted { id: want.id.get(), value: want.value.get(), at_least: want.at_least != 0 })
            .collect();
        let room = search.room.get();
        let most = usize::from(search.most.get()).min(MOST_RESULTS);
        let mut tally = Tally::default();
        let found: Vec<(u64, Listing)> = self
            .directory
            .search(realm, id, room, &wants, most, &mut tally)
            .into_iter()
            .map(|(host, listing)| (host, listing.clone()))
            .collect();

        let refused: String =
            tally.refused_by.iter().map(|(&key, count)| format!(" {}x{count}", number(key))).collect();
        if self.quiet.lets(About::Searches, now) {
            log!(
                "{} searched: {} listed, {} full, {} found{}{refused}",
                id_text(id),
                tally.listed,
                tally.full,
                found.len(),
                if refused.is_empty() { "" } else { ", refused by" }
            );
        }
        self.asked.push_back(Asked { when: seconds_now(), asker: id, room, wants, tally, found: found.len() });
        if self.asked.len() > ASKED_KEPT {
            self.asked.pop_front();
        }

        // Nothing fits still answers, or the asker would wait out its timeout
        // for each failed search.
        let pages: Vec<&[(u64, Listing)]> =
            if found.is_empty() { vec![&[]] } else { found.chunks(FOUND_PER_PAGE).collect() };
        let mut first = 0;
        for page in pages {
            let header = ResultsPage {
                search: search.search,
                total: Be16::new(found.len() as u16),
                first: Be16::new(first as u16),
                count: Be16::new(page.len() as u16),
                pad: [0; 2],
            };
            let mut bytes = header.as_bytes().to_vec();
            for (host, listing) in page {
                let one = Found {
                    host: Be64::new(*host),
                    session: listing.session,
                    open_public: Be32::new(listing.open_public),
                    open_private: Be32::new(listing.open_private),
                    filled_public: Be32::new(listing.filled_public),
                    filled_private: Be32::new(listing.filled_private),
                    rtt: Be16::new(listing.rtt),
                    pad: [0; 2],
                };
                bytes.extend_from_slice(one.as_bytes());
            }
            self.say(from, Kind::Results, 0, &bytes, 0);
            first += page.len();
        }
    }

    fn send_chunk(&mut self, to: SocketAddrV4, title: u32, body: &[u8]) {
        let Ok(ask) = FileAsk::read_from_bytes(body) else { return };
        let offset = ask.offset.get() as usize;
        let file = self.playlists.of(title);
        let Some(left) = file.get(offset..) else { return };
        let bytes = &left[..left.len().min(CHUNK_BYTES)];
        let header = FileChunk { total: Be32::new(file.len() as u32), offset: ask.offset };
        let mut chunk = header.as_bytes().to_vec();
        chunk.extend_from_slice(bytes);
        self.say(to, Kind::FileChunk, 0, &chunk, 0);
    }

    fn say(&mut self, to: SocketAddrV4, kind: Kind, peer: u64, bytes: &[u8], token: u64) {
        if bytes.len() > LARGEST - size_of::<Header>() {
            return;
        }
        let header = Header {
            magic: Be32::new(MAGIC),
            kind: kind as u8,
            pad: [0; 3],
            peer: Be64::new(peer),
            token: Be64::new(token),
        };
        self.packet.clear();
        self.packet.extend_from_slice(header.as_bytes());
        self.packet.extend_from_slice(bytes);
        // A datagram that cannot go is one lost on the way.
        let _ = self.socket.send_to(&self.packet, to);
    }
}
