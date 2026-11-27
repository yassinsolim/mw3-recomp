//! mw2-relay: the server players choose with the launcher's NETWORK SERVER.
//! Everyone of one title connected to the same relay sees each other's System
//! Link games and Xbox LIVE parties, and plays them through it: the relay hands
//! every packet on (protocol.rs), so the players need no open port and never
//! learn each other's address. It runs no match itself; whoever hosts one plays
//! in it. It also keeps the sessions they list for matchmaking (directory.rs),
//! and serves the playlists Find Game offers. It serves any title, each apart
//! from the others.
//!
//!   mw2-relay [--port N] [--public | --password TEXT] [--max N] [--per-address N]
//!             [--sessions-per-address N] [--rate N] [--total-rate N]
//!             [--max-payload N] [--playlists [TITLE=]FILE]... [--log-file FILE]
//!             [--status-port N] [--status-address ADDRESS]
//!
//! --port is the UDP port it listens on (3074), --password what a copy must
//! give to join (MW2_RELAY_PASSWORD in the environment, for a container).
//! --public says that there is none and anyone joins, which is also what no
//! password means; with a password it is a mistake, and the relay does not
//! start. --max is how many copies it takes at once (64), --per-address how
//! many of them from one IP address (16: players behind one router share
//! theirs), --sessions-per-address how many sessions those list (4; a copy
//! lists one). --rate is the packets a second it hands on for one copy (1000),
//! --total-rate for all of them (20000), --max-payload the bytes a packet may
//! carry after its header (2048); what is above is dropped, and logged, as
//! limits.rs has it. It keeps nothing but the copies it took in, each forgotten
//! when it leaves or goes quiet, with what they listed. --playlists is the file
//! every copy on the relay gets, read once at the start, or with TITLE= (a
//! title id in hexadecimal) every copy of that title; a title without one has
//! nothing for Find Game to offer. It logs a line for each copy that joins and
//! leaves, to the standard output and to --log-file if given, and stops on
//! SIGINT or SIGTERM. --status-port answers any TCP connection with what the
//! relay holds right now, as JSON, on --status-address: 127.0.0.1 unless told
//! otherwise, since it has no password. --perf-port takes the performance
//! reports of players who chose to send them (perf.rs, docs/perf-reports.md)
//! as HTTP on --perf-address, 127.0.0.1 unless told otherwise, and keeps them
//! in the file --perf-db names; MW2_RELAY_PERF_PORT and MW2_RELAY_PERF_DB in
//! the environment say the same, for a unit that a drop-in adds them to.
//! Reports that cannot be taken are a line of the log, never a relay that
//! does not start: the players come first.

mod directory;
mod limits;
mod log;
mod perf;
mod protocol;
mod relay;

use limits::Rate;
use log::log;
use protocol::*;
use relay::{Limits, Playlists, Relay};
use std::fs::{self, OpenOptions};
use std::io::{Read, Write};
use std::net::{Ipv4Addr, Shutdown, SocketAddr, TcpListener, UdpSocket};
use std::process::ExitCode;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

/// How long a wait for a packet lasts: how late a stop or a status answer can be.
const WAIT: Duration = Duration::from_millis(100);
/// The playlists a relay holds: one for each title, and one for the others.
const MOST_PLAYLISTS: usize = 8;
/// The least --max-payload: the longest Search, the longest packet of the
/// relay's own, fits.
const LEAST_PAYLOAD: usize = 512;

struct Options {
    port: u16,
    public: bool,
    password: String,
    most: usize,
    per_address: usize,
    sessions_per_address: usize,
    rate: u32,
    total_rate: u32,
    payload: usize,
    status_port: u16,
    status_address: Ipv4Addr,
    playlists: Vec<String>,
    log_file: Option<String>,
    perf_port: u16,
    perf_address: Ipv4Addr,
    perf_db: String,
}

const USAGE: &str = "usage: mw2-relay [--port N] [--public | --password TEXT] [--max N] [--per-address N]
                 [--sessions-per-address N] [--rate N] [--total-rate N]
                 [--max-payload N] [--playlists [TITLE=]FILE]... [--log-file FILE]
                 [--status-port N] [--status-address ADDRESS]
                 [--perf-port N --perf-db FILE [--perf-address ADDRESS]]";

fn options() -> Option<Options> {
    let mut options = Options {
        port: DEFAULT_PORT,
        public: false,
        password: std::env::var("MW2_RELAY_PASSWORD").unwrap_or_default(),
        most: 64,
        per_address: 16,
        sessions_per_address: 4,
        rate: 1000,
        total_rate: 20000,
        payload: LARGEST_PAYLOAD,
        status_port: 0,
        status_address: Ipv4Addr::LOCALHOST,
        playlists: Vec::new(),
        log_file: None,
        perf_port: std::env::var("MW2_RELAY_PERF_PORT").ok().and_then(|port| port.parse().ok()).unwrap_or(0),
        perf_address: Ipv4Addr::LOCALHOST,
        perf_db: std::env::var("MW2_RELAY_PERF_DB").unwrap_or_default(),
    };
    fn positive<Number: std::str::FromStr + Default + PartialEq>(value: &str) -> Option<Number> {
        value.parse().ok().filter(|number| *number != Number::default())
    }
    let mut arguments = std::env::args().skip(1);
    while let Some(option) = arguments.next() {
        if option == "--public" {
            options.public = true;
            continue;
        }
        let value = arguments.next()?;
        match option.as_str() {
            "--port" => options.port = positive(&value)?,
            "--password" => options.password = value,
            "--max" => options.most = positive(&value)?,
            "--per-address" => options.per_address = positive(&value)?,
            "--sessions-per-address" => options.sessions_per_address = positive(&value)?,
            "--rate" => options.rate = positive(&value)?,
            "--total-rate" => options.total_rate = positive(&value)?,
            "--max-payload" => {
                let most = LARGEST - size_of::<Header>();
                options.payload = value.parse().ok().filter(|payload| (LEAST_PAYLOAD..=most).contains(payload))?;
            }
            "--playlists" => options.playlists.push(value),
            "--log-file" => options.log_file = Some(value),
            "--status-port" => options.status_port = value.parse().ok()?,
            "--status-address" => options.status_address = value.parse().ok()?,
            "--perf-port" => options.perf_port = value.parse().ok()?,
            "--perf-address" => options.perf_address = value.parse().ok()?,
            "--perf-db" => options.perf_db = value,
            _ => return None,
        }
    }
    Some(options)
}

/// A --playlists value: the file, and the title it is for when it names one.
fn playlists_of(value: &str) -> (Option<u32>, &str) {
    if let Some((title, path)) = value.split_once('=')
        && let Ok(title) = u32::from_str_radix(title, 16)
    {
        return (Some(title), path);
    }
    (None, value)
}

/// Takes one connection, says `text` and closes it. Waits on nobody: what the
/// socket will not take at once is dropped, so a client that reads nothing
/// cannot hold up the players.
fn answer_status(listening: &TcpListener, relay: &Relay) {
    while let Ok((mut client, _)) = listening.accept() {
        if client.set_nonblocking(true).is_err() {
            continue;
        }
        let answer = format!(
            "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{}",
            relay.status()
        );
        let mut ignored = [0; 512];
        let _ = client.read(&mut ignored);
        let _ = client.write_all(answer.as_bytes());
        let _ = client.shutdown(Shutdown::Both);
    }
}

fn run(options: Options) -> Result<(), String> {
    let hello_password = size_of::<Hello>() - 12;
    if options.password.len() >= hello_password {
        return Err(format!("the password is longer than {} characters", hello_password - 1));
    }
    if options.public && !options.password.is_empty() {
        return Err("--public is a relay without a password, and it was given one".into());
    }
    if let Some(path) = &options.log_file {
        let file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(path)
            .map_err(|error| format!("cannot write {path}: {error}"))?;
        log::to_file(file);
    }
    if options.playlists.len() > MOST_PLAYLISTS {
        return Err(format!("more than {MOST_PLAYLISTS} playlists"));
    }
    let mut playlists = Playlists::default();
    for value in &options.playlists {
        let (title, path) = playlists_of(value);
        let file = fs::read(path).map_err(|error| format!("cannot read {path}: {error}"))?;
        if file.is_empty() || file.len() > LARGEST_FILE {
            return Err(format!("{path} has {} bytes; it must have 1 to {LARGEST_FILE}", file.len()));
        }
        if !playlists.add(title, file) {
            return Err(format!("{path} is a second playlists file for the same title"));
        }
    }

    let socket = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, options.port))
        .map_err(|error| format!("cannot listen on UDP port {}: {error}", options.port))?;
    socket.set_read_timeout(Some(WAIT)).map_err(|error| error.to_string())?;
    let receiving = socket.try_clone().map_err(|error| error.to_string())?;
    let mut status = None;
    if options.status_port != 0 {
        let listening = TcpListener::bind((options.status_address, options.status_port)).map_err(|error| {
            format!("cannot listen on {}, TCP port {}: {error}", options.status_address, options.status_port)
        })?;
        listening.set_nonblocking(true).map_err(|error| error.to_string())?;
        status = Some(listening);
    }
    if options.perf_port != 0 {
        let listening = TcpListener::bind((options.perf_address, options.perf_port))
            .map_err(|error| format!("cannot listen on {}, TCP port {}: {error}", options.perf_address, options.perf_port));
        let reports = if options.perf_db.is_empty() {
            Err("--perf-port without --perf-db: nowhere to keep them".to_string())
        } else {
            perf::Perf::open(&options.perf_db, Instant::now(), perf::today())
        };
        match (listening, reports) {
            (Ok(listening), Ok(reports)) => {
                // Its own thread: a report is written to a file, and the
                // players' packets do not wait for a disk.
                std::thread::Builder::new()
                    .name("perf".into())
                    .spawn(move || perf::serve(listening, reports))
                    .map_err(|error| error.to_string())?;
            }
            (Err(why), _) | (_, Err(why)) => log!("performance reports: not taken: {why}"),
        }
    }
    let stop = Arc::new(AtomicBool::new(false));
    for signal in [signal_hook::consts::SIGINT, signal_hook::consts::SIGTERM] {
        signal_hook::flag::register(signal, Arc::clone(&stop)).map_err(|error| error.to_string())?;
    }
    log!(
        "mw2-relay, protocol {PROTOCOL}, on UDP port {}, {}, at most {} copies, {} from one address listing {} \
         sessions, {} packets a second from one copy and {} from all, of {} bytes, {} playlists",
        options.port,
        if options.password.is_empty() { "public: anyone joins" } else { "with a password" },
        options.most,
        options.per_address,
        options.sessions_per_address,
        options.rate,
        options.total_rate,
        options.payload,
        playlists.count()
    );

    let limits = Limits {
        password: options.password,
        most: options.most,
        per_address: options.per_address,
        sessions_per_address: options.sessions_per_address,
        payload: options.payload,
        copy: Rate::of(options.rate),
        all: Rate::of(options.total_rate),
    };
    let mut relay = Relay::new(socket, limits, playlists);
    let mut packet = vec![0; LARGEST];
    let mut swept = Instant::now();
    let mut looked = Instant::now();
    while !stop.load(Ordering::Relaxed) {
        match receiving.recv_from(&mut packet) {
            Ok((size, SocketAddr::V4(from))) => relay.heard(&packet[..size], from),
            Ok(_) => {}
            // The wait ran out, or a signal ended it; the loop's own checks follow.
            Err(_) => {}
        }
        let now = Instant::now();
        if let Some(listening) = &status
            && now.duration_since(looked) >= WAIT
        {
            looked = now;
            answer_status(listening, &relay);
        }
        if now.duration_since(swept) >= Duration::from_secs(1) {
            relay.sweep();
            swept = now;
        }
    }
    log!("stopped");
    Ok(())
}

fn main() -> ExitCode {
    let Some(options) = options() else {
        eprintln!("{USAGE}");
        return ExitCode::from(2);
    };
    match run(options) {
        Ok(()) => ExitCode::SUCCESS,
        Err(why) => {
            eprintln!("mw2-relay: {why}");
            ExitCode::FAILURE
        }
    }
}
