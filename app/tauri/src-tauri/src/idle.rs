//! Output-idle detection: the "claude finished a long task" signal that
//! works regardless of pane count (design: app/design/notifications.md).
//!
//! The bell is the other trigger and lives in the webview (xterm's onBell),
//! because BEL is also the OSC terminator — `ESC ] 0 ; title BEL` sets a
//! window title — so scanning raw output bytes for 0x07 here would ring on
//! every title change. xterm's parser only reports a BEL that actually
//! rings. This machine covers what the bell cannot: sessions where the
//! child never rings, and composited multi-pane frames that drop BEL
//! (protocol-notes.md trap #1, until MSG_PANE_BELL).
//!
//! Pure state machine: no timers in the logic, the caller delivers ticks.
//! `step()` is the entire behaviour, so the tests enumerate transitions as
//! a table and cargo-mutants-style hand mutation has one target.
//!
//! ```text
//! IDLE   --output-->                          ACTIVE
//! ACTIVE --output streak >= busy_threshold--> BUSY
//! ACTIVE --silence >= settle-->               IDLE        (short burst: no notify)
//! BUSY   --silence >= done_threshold-->       IDLE + NOTIFY
//! ```
//!
//! The busy threshold (sustained output with gaps < `gap_max`) exists so a
//! one-line `ls` in a shell never notifies; only a session that was
//! demonstrably *working* announces completion.

use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug)]
pub struct Config {
    /// Recurring output for this long (gaps < `gap_max`) means BUSY.
    pub busy_threshold: Duration,
    /// A gap this long or longer restarts the busy streak.
    pub gap_max: Duration,
    /// Silence that ends a short ACTIVE burst without notifying.
    pub settle: Duration,
    /// Silence that ends BUSY — this is the notification.
    pub done_threshold: Duration,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            busy_threshold: Duration::from_secs(10),
            gap_max: Duration::from_secs(2),
            settle: Duration::from_secs(5),
            done_threshold: Duration::from_secs(5),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Idle,
    Active {
        streak_start: Instant,
        last_output: Instant,
    },
    Busy {
        last_output: Instant,
    },
}

#[derive(Clone, Copy, Debug)]
pub struct Machine {
    cfg: Config,
    state: State,
}

impl Machine {
    pub fn new(cfg: Config) -> Self {
        Machine {
            cfg,
            state: State::Idle,
        }
    }

    /// Output bytes arrived at `now`. Never notifies — output is the
    /// opposite of "finished".
    pub fn on_output(&mut self, now: Instant) {
        self.state = match self.state {
            State::Idle => State::Active {
                streak_start: now,
                last_output: now,
            },
            State::Active {
                streak_start,
                last_output,
            } => {
                if now.duration_since(last_output) >= self.cfg.gap_max {
                    // The streak broke; this output starts a new one.
                    State::Active {
                        streak_start: now,
                        last_output: now,
                    }
                } else if now.duration_since(streak_start) >= self.cfg.busy_threshold {
                    State::Busy { last_output: now }
                } else {
                    State::Active {
                        streak_start,
                        last_output: now,
                    }
                }
            }
            State::Busy { .. } => State::Busy { last_output: now },
        };
    }

    /// Time passed; `now` is the caller's clock. Returns true exactly when
    /// a BUSY session has been silent for `done_threshold` — the one
    /// transition that means "finished".
    #[must_use]
    pub fn on_tick(&mut self, now: Instant) -> bool {
        match self.state {
            State::Idle => false,
            State::Active { last_output, .. } => {
                if now.duration_since(last_output) >= self.cfg.settle {
                    self.state = State::Idle;
                }
                false
            }
            State::Busy { last_output } => {
                if now.duration_since(last_output) >= self.cfg.done_threshold {
                    self.state = State::Idle;
                    true
                } else {
                    false
                }
            }
        }
    }

    /// When the caller should next deliver a tick: the earliest instant at
    /// which `on_tick` could change anything. None while IDLE — with no
    /// output there is nothing to time out.
    pub fn next_deadline(&self) -> Option<Instant> {
        match self.state {
            State::Idle => None,
            State::Active { last_output, .. } => Some(last_output + self.cfg.settle),
            State::Busy { last_output } => Some(last_output + self.cfg.done_threshold),
        }
    }

    #[cfg(test)]
    pub fn state(&self) -> State {
        self.state
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cfg() -> Config {
        Config::default()
    }

    /// Drive a machine with a script of (seconds, event) rows.
    /// O = output, T = tick. Returns every second at which a tick notified.
    fn run(script: &[(u64, char)]) -> Vec<u64> {
        let t0 = Instant::now();
        let mut m = Machine::new(cfg());
        let mut notified = Vec::new();
        for &(secs, ev) in script {
            let now = t0 + Duration::from_secs(secs);
            match ev {
                'O' => m.on_output(now),
                'T' => {
                    if m.on_tick(now) {
                        notified.push(secs);
                    }
                }
                _ => unreachable!(),
            }
        }
        notified
    }

    #[test]
    fn long_work_then_silence_notifies_once() {
        // Output every second for 12 s (busy at 10), then silence; the
        // tick at +5 s of silence notifies, later ticks stay quiet.
        let mut script: Vec<(u64, char)> = (0..=12).map(|s| (s, 'O')).collect();
        script.extend([(15, 'T'), (17, 'T'), (18, 'T'), (25, 'T')]);
        assert_eq!(run(&script), vec![17]);
    }

    #[test]
    fn short_burst_settles_without_notifying() {
        // `ls` in a shell: 2 s of output, then silence. Settle at +5 s,
        // no notification ever.
        assert_eq!(
            run(&[(0, 'O'), (1, 'O'), (2, 'O'), (7, 'T'), (30, 'T')]),
            Vec::<u64>::new()
        );
    }

    #[test]
    fn a_gap_restarts_the_busy_streak() {
        // 6 s of output, 3 s gap (>= gap_max), 6 more seconds: neither
        // streak reaches 10 s, so this never becomes BUSY and never
        // notifies — even after long silence.
        let mut script: Vec<(u64, char)> = (0..=6).map(|s| (s, 'O')).collect();
        script.extend((9..=15).map(|s| (s, 'O')));
        script.extend([(21, 'T'), (40, 'T')]);
        assert_eq!(run(&script), Vec::<u64>::new());
        // ...but 10 s of gap-free output after the restart does notify.
        let mut script: Vec<(u64, char)> = (0..=6).map(|s| (s, 'O')).collect();
        script.extend((9..=19).map(|s| (s, 'O')));
        script.push((25, 'T'));
        assert_eq!(run(&script), vec![25]);
    }

    #[test]
    fn a_gap_of_exactly_gap_max_restarts_too() {
        // Output every 2 s — each gap is exactly gap_max, so every output
        // restarts the streak and BUSY is never reached. A `>` where `>=`
        // belongs turns this steady drip into a busy session that notifies.
        let mut script: Vec<(u64, char)> = (0..=10).map(|s| (s * 2, 'O')).collect();
        script.push((30, 'T'));
        assert_eq!(run(&script), Vec::<u64>::new());
    }

    #[test]
    fn output_during_busy_postpones_the_notification() {
        let mut script: Vec<(u64, char)> = (0..=10).map(|s| (s, 'O')).collect();
        // 4 s of silence — not yet done — then more output, then real silence.
        script.extend([(14, 'T'), (14, 'O'), (18, 'T'), (19, 'O'), (24, 'T')]);
        assert_eq!(run(&script), vec![24]);
    }

    #[test]
    fn idle_ticks_are_inert() {
        assert_eq!(run(&[(0, 'T'), (100, 'T')]), Vec::<u64>::new());
    }

    #[test]
    fn deadline_matches_the_state() {
        let t0 = Instant::now();
        let mut m = Machine::new(cfg());
        assert_eq!(m.next_deadline(), None, "idle: nothing to time out");
        m.on_output(t0);
        assert_eq!(
            m.next_deadline(),
            Some(t0 + cfg().settle),
            "active: settle clock"
        );
        for s in 1..=10 {
            m.on_output(t0 + Duration::from_secs(s));
        }
        assert!(matches!(m.state(), State::Busy { .. }));
        assert_eq!(
            m.next_deadline(),
            Some(t0 + Duration::from_secs(10) + cfg().done_threshold),
            "busy: done clock from the LAST output"
        );
    }

    #[test]
    fn exactly_at_the_boundary_counts() {
        // duration_since == threshold must fire: a caller that ticks at
        // precisely the deadline it was given must not need a second tick.
        let t0 = Instant::now();
        let mut m = Machine::new(cfg());
        for s in 0..=10 {
            m.on_output(t0 + Duration::from_secs(s));
        }
        assert!(matches!(m.state(), State::Busy { .. }));
        assert!(m.on_tick(t0 + Duration::from_secs(10) + cfg().done_threshold));
    }
}
