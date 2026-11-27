//! Performance reports from players who chose to send them (docs/perf-reports.md,
//! PRIVACY.md): POST /perf takes one, GET /perf/summary answers what each
//! graphics chip sustains as JSON, GET /perf the same as a page, and GET
//! /perf/health says that reports are taken here, which is what the app asks
//! before it offers the choice.
//!
//! What a report may hold is the list below and nothing else: a report with
//! another field, or a value that is not of the field's kind, is refused whole.
//! The address a report comes from is used for one thing, how often it may
//! send, and is held for that as a number made of it with a key this run alone
//! knows: it is never written to the file, nor to the log, and is gone when the
//! relay stops. A row has the day it came on and no time. Rows older than
//! twelve months are deleted.
//!
//! It listens on 127.0.0.1 unless told otherwise, in plain HTTP: what faces the
//! players is a server in front of it that holds the certificate, and says
//! where each request came from in X-Forwarded-For. Nothing here may panic: the
//! relay is built to abort on one, and the players' matches would go with it.

use crate::limits::{Bucket, Rate};
use crate::log::log;
use rusqlite::Connection;
use serde_json::{Map, Value};
use siphasher::sip::SipHasher13;
use std::collections::HashMap;
use std::fmt::Write as _;
use std::hash::Hasher;
use std::io::{Read, Write};
use std::net::{IpAddr, Shutdown, TcpListener, TcpStream};
use std::time::{Duration, Instant};

/// The most bytes a report may be; a real one is about 500.
pub const LARGEST_REPORT: usize = 2048;
/// The most bytes of a request's line and headers.
const LARGEST_HEAD: usize = 8192;
/// How long a client has to send its request and to take the answer.
const PATIENCE: Duration = Duration::from_secs(3);
/// Reports from one address: four at once, then six an hour. A session is a
/// report, and a household behind one router plays a few an evening.
const FROM_ONE: Rate = Rate { a_second: 6.0 / 3600.0, at_once: 4.0 };
/// Reports from everyone: one a second, sixty at once.
const FROM_ALL: Rate = Rate { a_second: 1.0, at_once: 60.0 };
/// The addresses whose rate is held at once; a full table refuses new ones.
const MOST_ADDRESSES: usize = 4096;
/// The rows the file may hold, about 200 MB: more are refused until the
/// oldest are deleted.
const MOST_ROWS: i64 = 1_000_000;
/// The days a report is kept: twelve months.
const KEPT_DAYS: i64 = 365;
/// A group is shown from this many reports of this many installs, so that no
/// single player's session can be read from the page.
pub const SMALLEST_GROUP: usize = 5;
/// How long a summary is answered from memory.
const SUMMARY_FOR: Duration = Duration::from_secs(600);

const TEXT_LONGEST: usize = 64;
const GAMES: [&str; 4] = ["mw3", "mw2", "cod4", "aw"];
const MODES: [&str; 2] = ["campaign", "multiplayer"];
const LIMITED_BY: [&str; 3] = ["gpu", "cpu", "unknown"];
const SHADOWS: [&str; 3] = ["auto", "on", "off"];

/// What a field of a report must be.
enum Kind {
    /// Letters, digits, spaces and a little punctuation, never a slash: a
    /// name, not a path.
    Text,
    OneOf(&'static [&'static str]),
    /// 32 hexadecimal digits.
    Install,
    Whole(i64, i64),
    Number(f64, f64),
}

/// Every field of a report, in the order of the table's columns after the day.
const FIELDS: [(&str, Kind); 22] = [
    ("install", Kind::Install),
    ("model", Kind::Text),
    ("chip", Kind::Text),
    ("gpu", Kind::Text),
    ("driver", Kind::Text),
    ("driver_version", Kind::Text),
    ("android", Kind::Text),
    ("app", Kind::Text),
    ("game", Kind::OneOf(&GAMES)),
    ("game_version", Kind::Text),
    ("mode", Kind::OneOf(&MODES)),
    ("map", Kind::Text),
    ("minutes", Kind::Whole(1, 1440)),
    ("fps", Kind::Number(1.0, 1000.0)),
    ("low1", Kind::Number(0.1, 1000.0)),
    ("limit", Kind::OneOf(&LIMITED_BY)),
    ("preset", Kind::Text),
    ("scene_scale", Kind::Whole(10, 100)),
    ("shadows", Kind::OneOf(&SHADOWS)),
    ("fps_limit", Kind::Whole(0, 1000)),
    ("multiplier", Kind::Whole(1, 4)),
    ("thermal", Kind::Whole(-1, 6)),
];
/// The report's own version, which is not kept: this is the first.
const VERSION: i64 = 1;

const TABLE: &str = "CREATE TABLE IF NOT EXISTS reports (
    day INTEGER NOT NULL, install TEXT NOT NULL, model TEXT NOT NULL, chip TEXT NOT NULL, gpu TEXT NOT NULL,
    driver TEXT NOT NULL, driver_version TEXT NOT NULL, android TEXT NOT NULL, app TEXT NOT NULL,
    game TEXT NOT NULL, game_version TEXT NOT NULL, mode TEXT NOT NULL, map TEXT NOT NULL,
    minutes INTEGER NOT NULL, fps REAL NOT NULL, low1 REAL NOT NULL, limited_by TEXT NOT NULL,
    preset TEXT NOT NULL, scene_scale INTEGER NOT NULL, shadows TEXT NOT NULL, fps_limit INTEGER NOT NULL,
    multiplier INTEGER NOT NULL, thermal INTEGER NOT NULL,
    UNIQUE (install, day, game, map, minutes, fps, low1));
CREATE INDEX IF NOT EXISTS reports_day ON reports (day);
CREATE INDEX IF NOT EXISTS reports_group ON reports (gpu, game, preset, scene_scale, shadows, fps_limit, multiplier);";
const INSERT: &str = "INSERT OR IGNORE INTO reports VALUES \
    (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23)";
/// The rows of the groups large enough to show, a group after another: the
/// file counts the installs, so that none of them is read into memory.
const GROUPED: &str = "SELECT gpu, game, preset, scene_scale, shadows, fps_limit, multiplier, fps, low1 FROM reports \
    WHERE (gpu, game, preset, scene_scale, shadows, fps_limit, multiplier) IN \
        (SELECT gpu, game, preset, scene_scale, shadows, fps_limit, multiplier FROM reports \
         GROUP BY gpu, game, preset, scene_scale, shadows, fps_limit, multiplier \
         HAVING count(*) >= ?1 AND count(DISTINCT install) >= ?1) \
    ORDER BY gpu, game, preset, scene_scale, shadows, fps_limit, multiplier";

fn text_allowed(text: &str) -> bool {
    text.len() <= TEXT_LONGEST
        && text.chars().all(|letter| letter.is_ascii_alphanumeric() || " ._+-(),:@#".contains(letter))
}

/// The report as the values of its columns, or why it is not one.
fn checked(body: &[u8]) -> Result<Vec<rusqlite::types::Value>, &'static str> {
    use rusqlite::types::Value as Column;
    let report: Map<String, Value> = serde_json::from_slice(body).map_err(|_| "not a JSON object")?;
    if report.len() != FIELDS.len() + 1 {
        return Err("a field too many or too few");
    }
    if report.get("v").and_then(Value::as_i64) != Some(VERSION) {
        return Err("not version 1");
    }
    let mut columns = Vec::with_capacity(FIELDS.len());
    for (name, kind) in &FIELDS {
        let value = report.get(*name).ok_or("a field is missing")?;
        columns.push(match kind {
            Kind::Text => Column::Text(value.as_str().filter(|text| text_allowed(text)).ok_or("a text is not a name")?.into()),
            Kind::OneOf(choices) => {
                Column::Text(value.as_str().filter(|text| choices.contains(text)).ok_or("not one of the choices")?.into())
            }
            Kind::Install => Column::Text(
                value
                    .as_str()
                    .filter(|text| text.len() == 32 && text.bytes().all(|digit| matches!(digit, b'0'..=b'9' | b'a'..=b'f')))
                    .ok_or("not an install id")?
                    .into(),
            ),
            Kind::Whole(least, most) => {
                Column::Integer(value.as_i64().filter(|number| (*least..=*most).contains(number)).ok_or("a number out of range")?)
            }
            Kind::Number(least, most) => {
                Column::Real(value.as_f64().filter(|number| (*least..=*most).contains(number)).ok_or("a number out of range")?)
            }
        });
    }
    Ok(columns)
}

/// The days since 1970 in UTC: all a row says of when it came.
pub fn today() -> i64 {
    time::OffsetDateTime::now_utc().unix_timestamp().div_euclid(86400)
}

pub struct Request {
    pub method: String,
    pub path: String,
    /// The last address of X-Forwarded-For: the one the server in front saw.
    pub forwarded: Option<String>,
    pub body: Vec<u8>,
}

pub struct Answer {
    pub status: u16,
    pub kind: &'static str,
    pub body: String,
}

impl Answer {
    fn json(status: u16, body: impl Into<String>) -> Answer {
        Answer { status, kind: "application/json", body: body.into() }
    }

    fn refusal(status: u16, why: &str) -> Answer {
        Answer::json(status, serde_json::json!({ "error": why }).to_string())
    }
}

pub struct Perf {
    file: Connection,
    rows: i64,
    swept: i64,
    key: (u64, u64),
    all: Bucket,
    addresses: HashMap<u64, Bucket>,
    summary: Option<(Instant, String)>,
}

impl Perf {
    /// Opens or makes the file at `path` (":memory:" for none) and deletes what is too old.
    pub fn open(path: &str, now: Instant, day: i64) -> Result<Perf, String> {
        let wrong = |error: rusqlite::Error| format!("cannot use {path}: {error}");
        let file = Connection::open(path).map_err(wrong)?;
        file.execute_batch(TABLE).map_err(wrong)?;
        let mut key = [0u8; 16];
        getrandom::fill(&mut key).map_err(|error| format!("no random numbers: {error}"))?;
        let (first, second) = key.split_at(8);
        let number = |half: &[u8]| half.iter().fold(0u64, |held, byte| held << 8 | u64::from(*byte));
        let mut perf = Perf {
            file,
            rows: 0,
            swept: i64::MIN,
            key: (number(first), number(second)),
            all: Bucket::full(FROM_ALL, now),
            addresses: HashMap::new(),
            summary: None,
        };
        perf.sweep(day).map_err(wrong)?;
        Ok(perf)
    }

    pub fn rows(&self) -> i64 {
        self.rows
    }

    /// Once a day: the rows older than twelve months go, and the rest are counted.
    fn sweep(&mut self, day: i64) -> Result<(), rusqlite::Error> {
        if day == self.swept {
            return Ok(());
        }
        self.file.execute("DELETE FROM reports WHERE day < ?1", [day - KEPT_DAYS])?;
        self.rows = self.file.query_row("SELECT count(*) FROM reports", [], |row| row.get(0))?;
        self.swept = day;
        Ok(())
    }

    /// True when a report from this address may be taken now. The address
    /// itself is not kept, only a number made of it with this run's key.
    fn lets(&mut self, address: &str, now: Instant) -> bool {
        let mut hasher = SipHasher13::new_with_keys(self.key.0, self.key.1);
        hasher.write(address.as_bytes());
        let number = hasher.finish();
        if !self.addresses.contains_key(&number) && self.addresses.len() >= MOST_ADDRESSES {
            // Those that have not sent for an hour hold a full bucket again,
            // and are forgotten: they would start with one anyway.
            let full_after = Duration::from_secs_f64(FROM_ONE.at_once / FROM_ONE.a_second);
            self.addresses.retain(|_, bucket| !bucket.quiet_for(full_after, now));
            if self.addresses.len() >= MOST_ADDRESSES {
                return false;
            }
        }
        let bucket = self.addresses.entry(number).or_insert_with(|| Bucket::full(FROM_ONE, now));
        bucket.take(FROM_ONE, 1.0, now) && self.all.take(FROM_ALL, 1.0, now)
    }

    fn take(&mut self, request: &Request, peer: IpAddr, now: Instant, day: i64) -> Answer {
        // The server in front is on this host; a request that comes from
        // anywhere else is counted under where it came from itself.
        let address = match &request.forwarded {
            Some(forwarded) if peer.is_loopback() => forwarded.clone(),
            _ => peer.to_string(),
        };
        if !self.lets(&address, now) {
            return Answer::refusal(429, "too many reports from this address; try again in an hour");
        }
        let columns = match checked(&request.body) {
            Ok(columns) => columns,
            Err(why) => return Answer::refusal(400, why),
        };
        if self.sweep(day).is_err() || self.rows >= MOST_ROWS {
            return Answer::refusal(503, "no room for reports now");
        }
        let row = std::iter::once(rusqlite::types::Value::Integer(day)).chain(columns);
        match self.file.execute(INSERT, rusqlite::params_from_iter(row)) {
            Ok(added) => {
                self.rows += i64::try_from(added).unwrap_or(0);
                Answer::json(200, r#"{"ok":true}"#)
            }
            Err(_) => Answer::refusal(503, "the report could not be kept"),
        }
    }

    /// Each group of at least SMALLEST_GROUP reports from as many installs:
    /// the chip, the game, the settings, and the medians of what it drew.
    fn groups(&self) -> Result<Vec<Value>, rusqlite::Error> {
        type Key = (String, String, String, i64, String, i64, i64);
        fn median(values: &mut [f64]) -> f64 {
            values.sort_by(f64::total_cmp);
            let middle = values.len() / 2;
            let upper = values.get(middle).copied().unwrap_or(0.0);
            if values.len() % 2 == 1 { upper } else { (values.get(middle.wrapping_sub(1)).copied().unwrap_or(upper) + upper) / 2.0 }
        }
        fn close(groups: &mut Vec<Value>, key: Option<Key>, fps: &mut Vec<f64>, low: &mut Vec<f64>) {
            if let Some((gpu, game, preset, scene_scale, shadows, fps_limit, multiplier)) = key {
                groups.push(serde_json::json!({
                    "gpu": gpu, "game": game, "preset": preset, "scene_scale": scene_scale, "shadows": shadows,
                    "fps_limit": fps_limit, "multiplier": multiplier,
                    "median_fps": (median(fps) * 10.0).round() / 10.0,
                    "median_low1": (median(low) * 10.0).round() / 10.0,
                    "reports": fps.len(),
                }));
            }
            fps.clear();
            low.clear();
        }
        let mut asked = self.file.prepare(GROUPED)?;
        let mut rows = asked.query([i64::try_from(SMALLEST_GROUP).unwrap_or(i64::MAX)])?;
        let (mut groups, mut key, mut fps, mut low) = (Vec::new(), None::<Key>, Vec::new(), Vec::new());
        while let Some(row) = rows.next()? {
            let of: Key = (row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?, row.get(4)?, row.get(5)?, row.get(6)?);
            if key.as_ref() != Some(&of) {
                close(&mut groups, key.replace(of), &mut fps, &mut low);
            }
            fps.push(row.get(7)?);
            low.push(row.get(8)?);
        }
        close(&mut groups, key, &mut fps, &mut low);
        Ok(groups)
    }

    fn summary(&mut self, now: Instant) -> Option<String> {
        if let Some((made, text)) = &self.summary
            && now.saturating_duration_since(*made) < SUMMARY_FOR
        {
            return Some(text.clone());
        }
        let text = serde_json::json!({ "smallest_group": SMALLEST_GROUP, "groups": self.groups().ok()? }).to_string();
        self.summary = Some((now, text.clone()));
        Some(text)
    }

    pub fn answer(&mut self, request: &Request, peer: IpAddr, now: Instant, day: i64) -> Answer {
        match (request.method.as_str(), request.path.as_str()) {
            ("POST", "/perf") => self.take(request, peer, now, day),
            ("GET", "/perf/health") => Answer::json(200, r#"{"ok":true}"#),
            ("GET", "/perf/summary") => match self.summary(now) {
                Some(text) => Answer::json(200, text),
                None => Answer::refusal(503, "the reports could not be read"),
            },
            ("GET", "/perf") | ("GET", "/perf/") => match self.summary(now) {
                Some(text) => Answer { status: 200, kind: "text/html; charset=utf-8", body: page(&text) },
                None => Answer::refusal(503, "the reports could not be read"),
            },
            (_, "/perf") | (_, "/perf/") | (_, "/perf/health") | (_, "/perf/summary") => Answer::refusal(405, "not that method"),
            _ => Answer::refusal(404, "nothing here"),
        }
    }
}

fn escaped(text: &str) -> String {
    text.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;")
}

/// The summary as a page: a table for each graphics chip.
fn page(summary: &str) -> String {
    let parsed: Value = serde_json::from_str(summary).unwrap_or(Value::Null);
    let groups = parsed.get("groups").and_then(Value::as_array).cloned().unwrap_or_default();
    let text = |group: &Value, field: &str| escaped(group.get(field).and_then(Value::as_str).unwrap_or(""));
    let number = |group: &Value, field: &str| group.get(field).map(Value::to_string).unwrap_or_default();
    let mut out = String::from(
        "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">\
         <title>How each chip runs the games</title><style>body{font:16px system-ui,sans-serif;margin:2em auto;\
         max-width:60em;padding:0 1em}table{border-collapse:collapse;width:100%}td,th{padding:.3em .6em;\
         border-bottom:1px solid #ccc;text-align:right}td:first-child,th:first-child{text-align:left}</style>\
         <h1>How each chip runs the games</h1><p>From players who chose to send anonymous performance reports. \
         A row is shown once it has at least ",
    );
    let _ = write!(
        out,
        "{SMALLEST_GROUP} reports from {SMALLEST_GROUP} installs, so no single player stands out. \
         <a href=\"https://github.com/yassinsolim/mw3-recomp/blob/main/PRIVACY.md\">What a report holds</a>; \
         the same as <a href=\"/perf/summary\">JSON</a>.</p>"
    );
    if groups.is_empty() {
        out.push_str("<p>No group has enough reports yet.</p>");
    }
    let mut chip = String::new();
    for group in &groups {
        let gpu = text(group, "gpu");
        if gpu != chip || chip.is_empty() {
            if !chip.is_empty() {
                out.push_str("</table>");
            }
            let _ = write!(
                out,
                "<h2>{gpu}</h2><table><tr><th>Game<th>Preset<th>Scene scale<th>Shadows<th>FPS limit<th>Multiplier\
                 <th>Median FPS<th>Median 1% low<th>Reports"
            );
            chip = gpu;
        }
        let limit = match group.get("fps_limit").and_then(Value::as_i64) {
            Some(0) | None => "auto".to_string(),
            Some(limit) => limit.to_string(),
        };
        let _ = write!(
            out,
            "<tr><td>{}<td>{}<td>{}%<td>{}<td>{limit}<td>{}x<td>{}<td>{}<td>{}",
            text(group, "game"),
            text(group, "preset"),
            number(group, "scene_scale"),
            text(group, "shadows"),
            number(group, "multiplier"),
            number(group, "median_fps"),
            number(group, "median_low1"),
            number(group, "reports"),
        );
    }
    if !chip.is_empty() {
        out.push_str("</table>");
    }
    out.push_str("</html>");
    out
}

/// A request off the socket, or the answer to one that cannot be read.
fn heard(client: &mut TcpStream) -> Result<Request, Answer> {
    let unreadable = || Answer::refusal(400, "not a request");
    let mut held = Vec::with_capacity(1024);
    let mut block = [0u8; 1024];
    let head = loop {
        if let Some(at) = held.windows(4).position(|window| window == b"\r\n\r\n") {
            break at;
        }
        if held.len() > LARGEST_HEAD {
            return Err(Answer::refusal(431, "the request's headers are too long"));
        }
        match client.read(&mut block) {
            Ok(read) if read > 0 => held.extend_from_slice(block.get(..read).unwrap_or_default()),
            _ => return Err(unreadable()),
        }
    };
    let text = std::str::from_utf8(held.get(..head).unwrap_or_default()).map_err(|_| unreadable())?;
    let mut lines = text.split("\r\n");
    let mut first = lines.next().unwrap_or_default().split(' ');
    let (method, target) = (first.next().unwrap_or_default().to_string(), first.next().ok_or_else(unreadable)?);
    let path = target.split('?').next().unwrap_or_default().to_string();
    let (mut length, mut forwarded) = (0usize, None);
    for line in lines {
        let Some((name, value)) = line.split_once(':') else { continue };
        if name.eq_ignore_ascii_case("content-length") {
            length = value.trim().parse().map_err(|_| unreadable())?;
        } else if name.eq_ignore_ascii_case("x-forwarded-for") {
            forwarded = value.rsplit(',').next().map(|last| last.trim().to_string()).filter(|last| !last.is_empty());
        }
    }
    if length > LARGEST_REPORT {
        return Err(Answer::refusal(413, "a report is at most 2048 bytes"));
    }
    let mut body = held.get(head + 4..).unwrap_or_default().to_vec();
    if body.len() > length {
        return Err(unreadable());
    }
    while body.len() < length {
        match client.read(&mut block) {
            Ok(read) if read > 0 && body.len() + read <= length => body.extend_from_slice(block.get(..read).unwrap_or_default()),
            _ => return Err(unreadable()),
        }
    }
    Ok(Request { method, path, forwarded, body })
}

fn said(client: &mut TcpStream, answer: &Answer) {
    let reason = match answer.status {
        200 => "OK",
        400 => "Bad Request",
        404 => "Not Found",
        405 => "Method Not Allowed",
        413 => "Content Too Large",
        429 => "Too Many Requests",
        431 => "Request Header Fields Too Large",
        _ => "Service Unavailable",
    };
    let head = format!(
        "HTTP/1.1 {} {reason}\r\nContent-Type: {}\r\nContent-Length: {}\r\nCache-Control: no-store\r\n\
         X-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'none'; style-src 'unsafe-inline'\r\n\
         Connection: close\r\n\r\n",
        answer.status,
        answer.kind,
        answer.body.len()
    );
    // A client that will not take its answer has none.
    let _ = client.write_all(head.as_bytes());
    let _ = client.write_all(answer.body.as_bytes());
    let _ = client.shutdown(Shutdown::Write);
}

/// Answers one client after another, on a thread of its own, until the relay
/// stops: a slow client holds up the next report for PATIENCE at most, and
/// never the players.
pub fn serve(listening: TcpListener, mut perf: Perf) {
    log!("performance reports: taken, {} held; no address is kept or logged", perf.rows());
    for client in listening.incoming() {
        let Ok(mut client) = client else { continue };
        let Ok(peer) = client.peer_addr() else { continue };
        if client.set_read_timeout(Some(PATIENCE)).is_err() || client.set_write_timeout(Some(PATIENCE)).is_err() {
            continue;
        }
        let answer = match heard(&mut client) {
            Ok(request) => perf.answer(&request, peer.ip(), Instant::now(), today()),
            Err(answer) => answer,
        };
        said(&mut client, &answer);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::Ipv4Addr;

    const HOME: IpAddr = IpAddr::V4(Ipv4Addr::LOCALHOST);

    fn report(install: u32, fps: f64) -> Value {
        serde_json::json!({
            "v": 1, "install": format!("{install:032x}"), "model": "AYN Thor", "chip": "QTI SM8550, board kalama",
            "gpu": "Adreno (TM) 740", "driver": "Turnip", "driver_version": "Mesa 25.3.0-devel (git-0a1b2c3)",
            "android": "13", "app": "0.1.7 (local)", "game": "mw3", "game_version": "disc", "mode": "multiplayer",
            "map": "mp_dome", "minutes": 12, "fps": fps, "low1": fps / 2.0, "limit": "gpu", "preset": "custom",
            "scene_scale": 70, "shadows": "auto", "fps_limit": 0, "multiplier": 1, "thermal": 2,
        })
    }

    fn post(body: &Value, from: &str) -> Request {
        Request { method: "POST".into(), path: "/perf".into(), forwarded: Some(from.into()), body: body.to_string().into_bytes() }
    }

    fn get(path: &str) -> Request {
        Request { method: "GET".into(), path: path.into(), forwarded: None, body: Vec::new() }
    }

    fn perf() -> (Perf, Instant) {
        let now = Instant::now();
        (Perf::open(":memory:", now, 20000).unwrap(), now)
    }

    #[test]
    fn a_report_is_kept_and_the_same_one_once() {
        let (mut perf, now) = perf();
        assert_eq!(perf.answer(&post(&report(1, 60.0), "203.0.113.7"), HOME, now, 20000).status, 200);
        assert_eq!(perf.answer(&post(&report(1, 60.0), "203.0.113.7"), HOME, now, 20000).status, 200, "sent again after a lost answer");
        assert_eq!(perf.rows(), 1);
    }

    #[test]
    fn only_the_listed_fields_are_taken() {
        let (mut perf, now) = perf();
        let mut refused = 0;
        let mut send = |body: Value| {
            refused += 1;
            perf.answer(&post(&body, &format!("203.0.113.{refused}")), HOME, now, 20000).status
        };
        let mut extra = report(1, 60.0);
        extra["ip"] = "203.0.113.7".into();
        assert_eq!(send(extra), 400, "a field that is not on the list");
        let mut missing = report(1, 60.0);
        missing.as_object_mut().unwrap().remove("thermal");
        assert_eq!(send(missing), 400);
        for (field, value) in [
            ("map", Value::from("/sdcard/Download/mp_dome.ff")),
            ("model", Value::from("C:\\Users\\someone")),
            ("model", Value::from("x".repeat(65))),
            ("gpu", Value::from("<script>")),
            ("install", Value::from("someone@example.com")),
            ("game", Value::from("bo3")),
            ("minutes", Value::from(0)),
            ("fps", Value::from(100000)),
            ("fps", Value::from("60")),
            ("v", Value::from(2)),
        ] {
            let mut wrong = report(1, 60.0);
            wrong[field] = value;
            assert_eq!(send(wrong), 400, "{field}");
        }
        assert_eq!(perf.rows(), 0);
        assert_eq!(perf.answer(&post(&Value::from("text"), "203.0.113.200"), HOME, now, 20000).status, 400);
    }

    #[test]
    fn an_address_sends_four_at_once_and_six_an_hour() {
        let (mut perf, now) = perf();
        let mut sent = |install: u32, from: &str, at: Instant| perf.answer(&post(&report(install, 60.0), from), HOME, at, 20000).status;
        for install in 0..4 {
            assert_eq!(sent(install, "203.0.113.7", now), 200);
        }
        assert_eq!(sent(4, "203.0.113.7", now), 429);
        assert_eq!(sent(5, "203.0.113.8", now), 200, "another address has its own");
        assert_eq!(sent(6, "203.0.113.7", now + Duration::from_secs(601)), 200, "one more every ten minutes");
        assert_eq!(sent(7, "203.0.113.7", now + Duration::from_secs(602)), 429);
        // Without the server in front, the address is the connection's own,
        // whatever the request says of itself.
        let direct = IpAddr::V4(Ipv4Addr::new(198, 51, 100, 9));
        for install in 10..14 {
            let from = format!("203.0.113.{install}");
            assert_eq!(perf.answer(&post(&report(install, 60.0), &from), direct, now, 20000).status, 200);
        }
        assert_eq!(perf.answer(&post(&report(14, 60.0), "203.0.113.99"), direct, now, 20000).status, 429);
    }

    #[test]
    fn a_full_table_of_addresses_refuses_new_ones_and_forgets_the_quiet() {
        let (mut perf, now) = perf();
        for address in 0..MOST_ADDRESSES as u64 {
            perf.addresses.insert(address, Bucket::full(FROM_ONE, now));
        }
        assert!(!perf.lets("one more", now));
        assert!(perf.lets("one more", now + Duration::from_secs(3600)));
        assert_eq!(perf.addresses.len(), 1);
    }

    #[test]
    fn everyone_together_sends_sixty_at_once() {
        let (mut perf, now) = perf();
        assert_eq!((0..100).filter(|address: &u32| perf.lets(&address.to_string(), now)).count(), 60);
    }

    #[test]
    fn the_file_holds_no_address() {
        let path = std::env::temp_dir().join(format!("perf-test-{}.sqlite", std::process::id()));
        let _ = std::fs::remove_file(&path);
        let now = Instant::now();
        let mut perf = Perf::open(path.to_str().unwrap(), now, 20000).unwrap();
        let direct = IpAddr::V4(Ipv4Addr::new(198, 51, 100, 9));
        assert_eq!(perf.answer(&post(&report(1, 60.0), "203.0.113.77"), HOME, now, 20000).status, 200);
        assert_eq!(perf.answer(&post(&report(2, 60.0), "ignored"), direct, now, 20000).status, 200);
        let columns: Vec<String> = perf
            .file
            .prepare("SELECT name FROM pragma_table_info('reports')")
            .unwrap()
            .query_map([], |row| row.get(0))
            .unwrap()
            .map(Result::unwrap)
            .collect();
        let mut expected = vec!["day".to_string()];
        expected.extend(FIELDS.iter().map(|(name, _)| if *name == "limit" { "limited_by".to_string() } else { name.to_string() }));
        assert_eq!(columns, expected, "the columns are the report's fields and the day");
        drop(perf);
        let bytes = std::fs::read(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        for address in ["203.0.113.77", "198.51.100.9"] {
            assert!(!bytes.windows(address.len()).any(|window| window == address.as_bytes()), "{address} is in the file");
        }
        assert!(bytes.windows(7).any(|window| window == b"mp_dome"), "the report itself is");
    }

    #[test]
    fn a_group_is_shown_from_five_reports_of_five_installs() {
        let (mut perf, now) = perf();
        let send = |install: u32, fps: f64, game: &str, perf: &mut Perf| {
            let mut body = report(install, fps);
            body["game"] = game.into();
            assert_eq!(perf.answer(&post(&body, &format!("203.0.113.{install}")), HOME, now, 20000).status, 200);
        };
        for install in 0..4 {
            send(install, 50.0 + f64::from(install), "mw3", &mut perf);
        }
        // Five reports of one install are one player.
        for session in 0..4 {
            send(100, 40.0 + f64::from(session), "aw", &mut perf);
        }
        let later = now + SUMMARY_FOR;
        let summary: Value = serde_json::from_str(&perf.answer(&get("/perf/summary"), HOME, later, 20000).body).unwrap();
        assert_eq!(summary["groups"].as_array().unwrap().len(), 0);
        send(4, 90.0, "mw3", &mut perf);
        send(101, 44.0, "aw", &mut perf);
        let cached: Value = serde_json::from_str(&perf.answer(&get("/perf/summary"), HOME, later, 20000).body).unwrap();
        assert_eq!(cached["groups"].as_array().unwrap().len(), 0, "answered from memory for ten minutes");
        let later = later + SUMMARY_FOR;
        let summary: Value = serde_json::from_str(&perf.answer(&get("/perf/summary"), HOME, later, 20000).body).unwrap();
        let groups = summary["groups"].as_array().unwrap();
        assert_eq!(groups.len(), 1, "{summary}");
        assert_eq!(groups[0]["game"], "mw3");
        assert_eq!(groups[0]["gpu"], "Adreno (TM) 740");
        assert_eq!(groups[0]["median_fps"], 52.0);
        assert_eq!(groups[0]["median_low1"], 26.0);
        assert_eq!(groups[0]["reports"], 5);
        assert!(groups[0].get("install").is_none() && groups[0].get("model").is_none());
        let page = perf.answer(&get("/perf"), HOME, later, 20000);
        assert_eq!(page.status, 200);
        assert!(page.body.contains("<h2>Adreno (TM) 740</h2>") && page.body.contains("<td>52.0<td>26.0<td>5"), "{}", page.body);
    }

    #[test]
    fn reports_older_than_twelve_months_are_deleted() {
        let (mut perf, now) = perf();
        assert_eq!(perf.answer(&post(&report(1, 60.0), "203.0.113.1"), HOME, now, 20000).status, 200);
        assert_eq!(perf.answer(&post(&report(2, 60.0), "203.0.113.2"), HOME, now, 20000 + KEPT_DAYS).status, 200);
        assert_eq!(perf.rows(), 2, "kept on its last day");
        assert_eq!(perf.answer(&post(&report(3, 60.0), "203.0.113.3"), HOME, now, 20001 + KEPT_DAYS).status, 200);
        assert_eq!(perf.rows(), 2, "the first is gone");
        let days: Vec<i64> =
            perf.file.prepare("SELECT day FROM reports ORDER BY day").unwrap().query_map([], |row| row.get(0)).unwrap().map(Result::unwrap).collect();
        assert_eq!(days, [20000 + KEPT_DAYS, 20001 + KEPT_DAYS]);
    }

    #[test]
    fn other_paths_and_methods_are_refused() {
        let (mut perf, now) = perf();
        assert_eq!(perf.answer(&get("/perf/health"), HOME, now, 20000).body, r#"{"ok":true}"#);
        assert_eq!(perf.answer(&get("/"), HOME, now, 20000).status, 404);
        assert_eq!(perf.answer(&get("/perf/reports"), HOME, now, 20000).status, 404);
        let mut delete = get("/perf");
        delete.method = "DELETE".into();
        assert_eq!(perf.answer(&delete, HOME, now, 20000).status, 405);
    }
}
