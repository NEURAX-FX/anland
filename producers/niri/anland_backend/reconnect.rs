//! Retry policy for real transport failures; a pending handshake has no timer.
use std::time::{Duration, Instant};

const MIN_DELAY: Duration = Duration::from_millis(100);
const MAX_DELAY: Duration = Duration::from_secs(2);

pub struct Reconnect {
    waiting: bool,
    retry_at: Option<Instant>,
    delay: Duration,
}

impl Reconnect {
    pub fn new() -> Self {
        Self { waiting: false, retry_at: None, delay: MIN_DELAY }
    }

    pub fn should_attempt(&self, now: Instant, ready: bool) -> bool {
        match self.retry_at {
            Some(deadline) => now >= deadline,
            None => !self.waiting || ready,
        }
    }

    pub fn waiting(&self) -> bool { self.waiting }
    pub fn backing_off(&self) -> bool { self.retry_at.is_some() }

    pub fn pending(&mut self) {
        self.waiting = true;
        self.retry_at = None;
    }

    pub fn connected(&mut self) {
        self.waiting = false;
        self.retry_at = None;
        self.delay = MIN_DELAY;
    }

    pub fn failed(&mut self, now: Instant) {
        self.waiting = false;
        self.retry_at = Some(now + self.delay);
        self.delay = (self.delay * 2).min(MAX_DELAY);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn healthy_pending_handshake_waits_for_readiness_without_deadline() {
        let now = Instant::now();
        let mut retry = Reconnect::new();
        assert!(retry.should_attempt(now, false));
        retry.pending();
        assert!(!retry.should_attempt(now + Duration::from_secs(3600), false));
        assert!(retry.should_attempt(now, true));
        assert!(!retry.backing_off());
    }

    #[test]
    fn hangup_readiness_cannot_bypass_backoff_and_recovery_resets_it() {
        let now = Instant::now();
        let mut retry = Reconnect::new();
        let mut at = now;
        for delay_ms in [100, 200, 400, 800, 1600, 2000, 2000] {
            retry.failed(at);
            assert!(retry.backing_off());
            assert!(!retry.should_attempt(at + Duration::from_millis(delay_ms - 1), true));
            at += Duration::from_millis(delay_ms);
            assert!(retry.should_attempt(at, false));
        }
        retry.pending();
        assert!(!retry.should_attempt(at + Duration::from_secs(60), false));
        retry.connected();
        retry.failed(at);
        assert!(!retry.should_attempt(at + Duration::from_millis(99), true));
        assert!(retry.should_attempt(at + Duration::from_millis(100), false));
    }
}
