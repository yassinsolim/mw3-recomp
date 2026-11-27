//! What keeps a relay anyone can reach standing: how fast an address may send,
//! and how much the log says about those that send faster. Nothing here holds
//! more than a fixed number of bytes, whatever is sent.

use crate::log::log;
use std::time::{Duration, Instant};

/// Packets a second, and how many may come at once after a silence.
#[derive(Clone, Copy)]
pub struct Rate {
    pub a_second: f64,
    pub at_once: f64,
}

impl Rate {
    /// Twice a second's worth at once: a game sends in bursts, at each frame.
    pub const fn of(a_second: u32) -> Rate {
        Rate { a_second: a_second as f64, at_once: a_second as f64 * 2.0 }
    }
}

/// A token bucket: full at first, filled at the rate, emptied by what passes.
pub struct Bucket {
    tokens: f64,
    filled: Instant,
}

impl Bucket {
    pub fn full(rate: Rate, now: Instant) -> Bucket {
        Bucket { tokens: rate.at_once, filled: now }
    }

    /// True when `cost` packets fit, which are then taken.
    pub fn take(&mut self, rate: Rate, cost: f64, now: Instant) -> bool {
        let since = now.saturating_duration_since(self.filled).as_secs_f64();
        self.tokens = (self.tokens + since * rate.a_second).min(rate.at_once);
        self.filled = now;
        if self.tokens < cost {
            return false;
        }
        self.tokens -= cost;
        true
    }

    /// True when nothing has been asked of it for `quiet`.
    pub fn quiet_for(&self, quiet: Duration, now: Instant) -> bool {
        now.saturating_duration_since(self.filled) >= quiet
    }
}

/// What a log line is about. Each has its own share of the log, so a flood of
/// one says nothing less about the others.
#[derive(Clone, Copy)]
pub enum About {
    Joining,
    Refusals,
    Limits,
    Listings,
    Searches,
}

const ABOUT: [&str; 5] = ["joining and leaving", "refusals", "limits", "listings", "searches"];
const LINES: u32 = 20;
const EVERY: Duration = Duration::from_secs(10);

struct Share {
    since: Instant,
    written: u32,
    held: u64,
}

/// At most LINES lines about each thing every EVERY, then a count of the rest:
/// whoever floods the relay does not fill its disk through the log.
pub struct Quiet {
    shares: [Share; ABOUT.len()],
}

impl Quiet {
    pub fn new(now: Instant) -> Quiet {
        Quiet { shares: std::array::from_fn(|_| Share { since: now, written: 0, held: 0 }) }
    }

    /// True when a line about this may be logged now.
    pub fn lets(&mut self, about: About, now: Instant) -> bool {
        let share = &mut self.shares[about as usize];
        Quiet::turn(share, ABOUT[about as usize], now);
        if share.written < LINES {
            share.written += 1;
            return true;
        }
        share.held += 1;
        false
    }

    /// Says how many lines were held back, once their time is over.
    pub fn sweep(&mut self, now: Instant) {
        for (share, about) in self.shares.iter_mut().zip(ABOUT) {
            Quiet::turn(share, about, now);
        }
    }

    fn turn(share: &mut Share, about: &str, now: Instant) {
        if now.saturating_duration_since(share.since) < EVERY {
            return;
        }
        if share.held != 0 {
            log!("{} more lines about {about} not logged", share.held);
        }
        *share = Share { since: now, written: 0, held: 0 };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_bucket_lets_a_burst_then_the_rate() {
        let rate = Rate::of(10);
        let start = Instant::now();
        let mut bucket = Bucket::full(rate, start);
        assert_eq!((0..100).filter(|_| bucket.take(rate, 1.0, start)).count(), 20);
        let later = start + Duration::from_millis(500);
        assert_eq!((0..100).filter(|_| bucket.take(rate, 1.0, later)).count(), 5);
        let much_later = later + Duration::from_secs(3600);
        assert_eq!((0..100).filter(|_| bucket.take(rate, 1.0, much_later)).count(), 20, "never more than at once");
    }

    #[test]
    fn a_bucket_takes_nothing_for_what_does_not_fit() {
        let rate = Rate::of(10);
        let start = Instant::now();
        let mut bucket = Bucket::full(rate, start);
        assert!(!bucket.take(rate, 21.0, start));
        assert!(bucket.take(rate, 20.0, start));
        assert!(!bucket.take(rate, 1.0, start));
    }

    #[test]
    fn the_log_has_a_share_for_each_thing() {
        let start = Instant::now();
        let mut quiet = Quiet::new(start);
        assert_eq!((0..1000).filter(|_| quiet.lets(About::Limits, start)).count(), LINES as usize);
        assert!(quiet.lets(About::Refusals, start), "a flood of one holds back no other");
        assert!(!quiet.lets(About::Limits, start + EVERY / 2));
        assert!(quiet.lets(About::Limits, start + EVERY));
    }
}
