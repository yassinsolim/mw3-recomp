//! The performance reports as the app and the server in front see them: the
//! real program on a real socket (src/perf.rs, docs/perf-reports.md). What a
//! report may hold and how groups are made is tested beside the code; here,
//! that nothing of where a report came from is left behind.

use std::io::{Read, Write};
use std::net::TcpStream;
use std::process::{Child, Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

const FROM: &str = "203.0.113.50";

struct Relay {
    child: Child,
    perf: u16,
    file: std::path::PathBuf,
}

impl Relay {
    fn start() -> Relay {
        // Above conformance.rs's ports, which share the machine.
        let port = 33000 + (std::process::id() % 500) as u16 * 2;
        let file = std::env::temp_dir().join(format!("mw2-relay-perf-{}.sqlite", std::process::id()));
        let _ = std::fs::remove_file(&file);
        let child = Command::new(env!("CARGO_BIN_EXE_mw2-relay"))
            .args(["--public", "--port", &port.to_string(), "--perf-port", &(port + 1).to_string()])
            .args(["--perf-db", file.to_str().unwrap()])
            .env_remove("MW2_RELAY_PASSWORD")
            .stdout(Stdio::piped())
            .spawn()
            .unwrap();
        let relay = Relay { child, perf: port + 1, file };
        let started = Instant::now();
        while TcpStream::connect(("127.0.0.1", relay.perf)).is_err() {
            assert!(started.elapsed() < Duration::from_secs(10), "the relay does not take reports");
            thread::sleep(Duration::from_millis(50));
        }
        relay
    }

    /// The status and the body of the answer to one request.
    fn ask(&self, method: &str, path: &str, body: &str) -> (u16, String) {
        let mut stream = TcpStream::connect(("127.0.0.1", self.perf)).unwrap();
        stream.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
        let request = format!(
            "{method} {path} HTTP/1.1\r\nHost: mw3.yassin.app\r\nX-Forwarded-For: {FROM}\r\nContent-Type: application/json\r\n\
             Content-Length: {}\r\n\r\n{body}",
            body.len()
        );
        // A request the relay refuses by its length is answered before it is
        // all sent, and the rest has nowhere to go.
        let _ = stream.write_all(request.as_bytes());
        let mut answer = String::new();
        let _ = stream.read_to_string(&mut answer);
        let status = answer.split(' ').nth(1).and_then(|status| status.parse().ok()).unwrap_or(0);
        (status, answer.split_once("\r\n\r\n").map(|(_, body)| body.to_string()).unwrap_or_default())
    }
}

fn report(install: u32) -> String {
    format!(
        r#"{{"v":1,"install":"{install:032x}","model":"Retroid Pocket Flip 2","chip":"QTI SM8250, board kona","gpu":"Adreno (TM) 650","driver":"Turnip","driver_version":"Mesa 25.3.0","android":"13","app":"0.1.7 (local)","game":"mw2","game_version":"disc","mode":"campaign","map":"trainer","minutes":7,"fps":58.4,"low1":41.0,"limit":"cpu","preset":"automatic","scene_scale":70,"shadows":"auto","fps_limit":60,"multiplier":1,"thermal":1}}"#
    )
}

#[test]
fn reports_are_taken_capped_and_limited_and_no_address_is_left() {
    let mut relay = Relay::start();
    assert_eq!(relay.ask("GET", "/perf/health", ""), (200, r#"{"ok":true}"#.to_string()));
    assert_eq!(relay.ask("GET", "/", "").0, 404);

    // The size cap: refused by its declared length, before any of it is read.
    let large = format!(r#"{{"v":1,"model":"{}"}}"#, "x".repeat(4000));
    assert_eq!(relay.ask("POST", "/perf", &large).0, 413);
    assert_eq!(relay.ask("POST", "/perf", r#"{"v":1}"#).0, 400);

    // The rate limit: the one that was not a report was of the address's
    // four at once; the one too large was refused before it counted.
    assert_eq!(relay.ask("POST", "/perf", &report(1)).0, 200);
    assert_eq!(relay.ask("POST", "/perf", &report(2)).0, 200);
    assert_eq!(relay.ask("POST", "/perf", &report(3)).0, 200);
    assert_eq!(relay.ask("POST", "/perf", &report(4)).0, 429);

    let (status, summary) = relay.ask("GET", "/perf/summary", "");
    assert_eq!(status, 200);
    assert!(summary.contains(r#""groups":[]"#), "three reports are no group: {summary}");
    let (status, page) = relay.ask("GET", "/perf", "");
    assert_eq!(status, 200);
    assert!(page.contains("No group has enough reports yet"), "{page}");

    relay.child.kill().unwrap();
    let mut said = String::new();
    relay.child.stdout.take().unwrap().read_to_string(&mut said).unwrap();
    relay.child.wait().unwrap();
    assert!(said.contains("performance reports: taken"), "{said}");
    assert!(!said.contains(FROM), "the log names the address: {said}");

    let mut kept = Vec::new();
    for name in ["", "-wal", "-journal"] {
        let mut path = relay.file.clone().into_os_string();
        path.push(name);
        kept.extend(std::fs::read(&path).unwrap_or_default());
        let _ = std::fs::remove_file(&path);
    }
    assert!(kept.windows(7).any(|window| window == b"trainer"), "the reports are in the file");
    assert!(!kept.windows(FROM.len()).any(|window| window == FROM.as_bytes()), "the file holds the address");
}
