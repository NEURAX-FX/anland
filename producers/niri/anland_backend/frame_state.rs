//! niri frame-loop bridge state, independent of the compositor and transport.
//! The niri adapter feeds *validated* scene events here in dispatch order.
use std::collections::VecDeque;
use std::time::Duration;

#[derive(Default, Debug)]
pub struct FrameState {
    generation: Option<u64>,
    target: Option<(u64, u32)>,
    pending: Option<u64>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FrameAction {
    None,
    QueueRedraw,
    Complete(u64),
    Cancel(u64),
}

impl FrameState {
    pub fn new() -> Self {
        Self::default()
    }

    /// Discard references to the old consumer before importing the new set.
    pub fn reset(&mut self, generation: u64) -> FrameAction {
        self.generation = Some(generation);
        self.target = None;
        self.pending
            .take()
            .map_or(FrameAction::None, FrameAction::Cancel)
    }

    pub fn target_ready(&mut self, generation: u64, index: u32, count: u32) -> FrameAction {
        if self.generation != Some(generation) || count == 0 || index >= count {
            return FrameAction::None;
        }
        self.target = Some((generation, index));
        // A selected target is permission to draw, *not* presentation of a frame.
        FrameAction::QueueRedraw
    }

    pub fn target(&self) -> Option<(u64, u32)> {
        if self.pending.is_some() {
            None
        } else {
            self.target
        }
    }

    pub fn submitted(&mut self, generation: u64, index: u32, commit_id: u64) -> bool {
        if commit_id == 0
            || self.pending.is_some()
            || self.target != Some((generation, index))
            || self.generation != Some(generation)
        {
            return false;
        }
        self.target = None;
        self.pending = Some(commit_id);
        true
    }

    pub fn consumed(&mut self, commit_id: u64) -> FrameAction {
        if self.pending == Some(commit_id) {
            self.pending = None;
            FrameAction::Complete(commit_id)
        } else {
            FrameAction::None
        }
    }

    pub fn dropped(&mut self, commit_id: u64) -> FrameAction {
        if self.pending == Some(commit_id) {
            self.pending = None;
            FrameAction::Cancel(commit_id)
        } else {
            FrameAction::None
        }
    }
}

// Android may poll for EGL's timestamp for up to 250 ms after swap. Allow
// transport/dispatch delay, but don't let a missing report retain callbacks
// forever (including when paired with an older APK).
const REPORT_TIMEOUT: Duration = Duration::from_millis(500);
const MAX_PENDING_REPORTS: usize = 64;

struct PendingReport<T> {
    serial: u64,
    commit_id: u64,
    sent_after: Duration,
    feedback: T,
}

/// Separate from FrameState: reporting display timing never gates rendering.
pub struct PresentationReports<T> {
    serial: u64,
    last_presentation: Duration,
    pending: VecDeque<PendingReport<T>>,
}

impl<T> PresentationReports<T> {
    pub fn new() -> Self {
        Self { serial: 0, last_presentation: Duration::ZERO, pending: VecDeque::new() }
    }

    pub fn reset(&mut self) {
        self.serial = 0;
        self.last_presentation = Duration::ZERO;
        self.pending.clear();
    }

    /// Record exactly one successful send on this session's fence channel.
    pub fn submitted(&mut self, commit_id: u64, sent_after: Duration, feedback: T) -> bool {
        let Some(serial) = self.serial.checked_add(1) else { return false; };
        self.serial = serial;
        if self.pending.len() == MAX_PENDING_REPORTS {
            self.pending.pop_front();
        }
        self.pending.push_back(PendingReport { serial, commit_id, sent_after, feedback });
        true
    }

    pub fn expire(&mut self, now: Duration) {
        while self.pending.front().is_some_and(|p| now.saturating_sub(p.sent_after) >= REPORT_TIMEOUT) {
            self.pending.pop_front();
        }
    }

    pub fn discard(&mut self, commit_id: u64) {
        if let Some(i) = self.pending.iter().position(|p| p.commit_id == commit_id) {
            self.pending.remove(i);
        }
    }

    pub fn reported(&mut self, serial: u64, ns: u64, now: Duration) -> Option<(Duration, T, bool)> {
        self.expire(now);
        let i = self.pending.iter().position(|p| p.serial == serial)?;
        let report = self.pending.remove(i)?;
        let time = Duration::from_nanos(ns);
        if ns == 0 || time < report.sent_after || time > now {
            return None;
        }

        // Timestamps can resolve out of order. Deliver this frame's feedback,
        // but never let an older timestamp rewind the compositor's frame clock.
        let update_clock = time > self.last_presentation;
        if update_clock {
            self.last_presentation = time;
        }
        Some((time, report.feedback, update_clock))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const fn ms(value: u64) -> Duration {
        Duration::from_millis(value)
    }

    #[test]
    fn present_report_never_unlocks_a_busy_slot() {
        let mut frame = FrameState::new();
        let mut reports = PresentationReports::new();
        frame.reset(1);
        assert_eq!(frame.target_ready(1, 0, 1), FrameAction::QueueRedraw);
        assert!(frame.submitted(1, 0, 42));
        assert!(reports.submitted(42, ms(1), 42));
        assert_eq!(reports.reported(1, 2_000_000, ms(3)), Some((ms(2), 42, true)));
        assert_eq!(frame.target(), None);
        assert_eq!(frame.consumed(42), FrameAction::Complete(42));
        assert_eq!(frame.target(), None);
        frame.target_ready(1, 0, 1);
        assert_eq!(frame.target(), Some((1, 0)));
    }

    #[test]
    fn out_of_order_reports_do_not_rewind_the_clock() {
        let mut reports = PresentationReports::new();
        assert!(reports.submitted(41, ms(10), 41));
        assert!(reports.submitted(42, ms(20), 42));
        assert_eq!(reports.reported(2, 30_000_000, ms(40)), Some((ms(30), 42, true)));
        assert_eq!(reports.reported(1, 15_000_000, ms(41)), Some((ms(15), 41, false)));
        assert_eq!(reports.reported(1, 15_000_000, ms(42)), None);
    }

    #[test]
    fn expires_discarded_and_invalid_frames_without_presenting() {
        let mut reports = PresentationReports::new();
        reports.submitted(1, ms(1), 1);
        reports.submitted(2, ms(2), 2);
        reports.submitted(3, ms(3), 3);
        reports.submitted(4, ms(4), 4);
        reports.submitted(5, ms(5), 5);
        reports.discard(1);
        assert_eq!(reports.reported(1, 2_000_000, ms(6)), None);
        assert_eq!(reports.reported(2, 0, ms(6)), None);
        assert_eq!(reports.reported(3, 2_000_000, ms(6)), None);
        assert_eq!(reports.reported(4, 7_000_000, ms(6)), None);
        assert_eq!(reports.reported(5, 6_000_000, ms(505)), None);

        reports.submitted(6, ms(510), 6);
        reports.reset();
        assert_eq!(reports.reported(6, 511_000_000, ms(512)), None);
        reports.submitted(7, ms(513), 7);
        assert_eq!(reports.reported(1, 514_000_000, ms(515)), Some((ms(514), 7, true)));
    }
}