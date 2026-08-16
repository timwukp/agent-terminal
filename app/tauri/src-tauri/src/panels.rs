//! One push stream for the filesystem-backed panels (usage, hooks, hook
//! log), replacing three independent `setInterval(2000)` pollers in the
//! webview.
//!
//! What this removes is *traffic*, not disk work. The reads were already
//! incremental — the hooks snapshot is guarded by an (mtime, len) cache
//! and normally costs one `stat`, the hook log advances a byte offset —
//! so the cost being paid twice a second per open panel was an IPC
//! round-trip carrying a snapshot the webview usually already had. Here
//! the same poll happens once, in one place, and a frame goes out only
//! when a snapshot's serialization actually differs from the last one
//! sent. The 2 s cadence is unchanged, so nothing becomes staler.
//!
//! Deliberately still a timer, not a filesystem watcher. `notify` is not
//! a dependency of this build and adding one to save a `stat` every two
//! seconds is not a trade worth a new crate; what matters is that the
//! contract the webview sees is now "frames arrive when things change",
//! so the producer can become a watcher later without the frontend
//! knowing.
//!
//! Frames are `InvokeResponseBody::Json`: `{"kind": …, "data": …}`. The
//! terminal channel's tag-byte framing (session.rs) exists because
//! terminal output is arbitrary bytes that must never be JSON-parsed;
//! panel data is JSON all the way down, so there is nothing to route
//! around.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{self, RecvTimeoutError};
use std::sync::Mutex;
use std::time::Duration;

use serde::{Deserialize, Serialize};
use tauri::ipc::{Channel, InvokeResponseBody};
use tauri::Manager;

/// Poll cadence behind the stream — the same 2 s the panels used, so a
/// change reaches the panel no later than it did before.
const TICK: Duration = Duration::from_secs(2);

/// What a client can ask to be streamed.
///
/// An enum rather than free strings so a typo from the webview fails at
/// the wire edge (the command rejects) instead of producing a panel that
/// simply never updates — a silence that looks exactly like "nothing
/// changed".
#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Kind {
    Usage,
    Hooks,
    HookLog,
}

impl Kind {
    /// The `kind` field the webview routes on (src/panels/panelStream.ts).
    fn tag(self) -> &'static str {
        match self {
            Kind::Usage => "usage",
            Kind::Hooks => "hooks",
            Kind::HookLog => "hook_log",
        }
    }
}

/// The last serialization sent per kind, so an unchanged snapshot costs
/// nothing on the wire.
///
/// Stores the payload itself rather than a hash of it. The gate never
/// re-sends what it believes the webview has, so a 64-bit collision
/// would not delay an update — it would drop it permanently, and the
/// panel would sit there showing the wrong number with no way back. A
/// few KB per kind is a cheap price for not having to reason about that.
#[derive(Default)]
pub struct ChangeGate {
    usage: Option<String>,
    hooks: Option<String>,
    hook_log: Option<String>,
}

impl ChangeGate {
    fn slot(&mut self, kind: Kind) -> &mut Option<String> {
        match kind {
            Kind::Usage => &mut self.usage,
            Kind::Hooks => &mut self.hooks,
            Kind::HookLog => &mut self.hook_log,
        }
    }

    /// True — and remembers `payload` — when `payload` differs from what
    /// was last accepted for `kind`. False means the webview already has
    /// this exact snapshot and the frame can be skipped.
    pub fn accept(&mut self, kind: Kind, payload: &str) -> bool {
        let slot = self.slot(kind);
        if slot.as_deref() == Some(payload) {
            return false;
        }
        *slot = Some(payload.to_string());
        true
    }
}

/// Serialize one snapshot as the frame the webview routes on.
pub fn frame<T: Serialize>(kind: Kind, data: &T) -> Result<String, serde_json::Error> {
    #[derive(Serialize)]
    struct Frame<'a, T> {
        kind: &'a str,
        data: &'a T,
    }
    serde_json::to_string(&Frame {
        kind: kind.tag(),
        data,
    })
}

/// A stop request only ends the stream it names.
///
/// `panel_stream` and `panel_stream_stop` are separate IPC calls with no
/// ordering guarantee between them, so an unconditional stop can arrive
/// *after* the next start and kill a live stream. That interleaving is
/// not exotic: React StrictMode mounts, unmounts and remounts every
/// effect, so it is what every dev-mode mount does.
pub fn stop_applies(current: Option<u64>, requested: u64) -> bool {
    current == Some(requested)
}

/// The stream this window is running. Dropping `_stop` ends its thread on
/// the next wakeup, which is how a replacement takes over.
pub struct Running {
    id: u64,
    _stop: mpsc::Sender<()>,
}

#[derive(Default)]
pub struct PanelStreamState(pub Mutex<Option<Running>>);

static NEXT_ID: AtomicU64 = AtomicU64::new(1);

/// One tick's frame for `kind`, or `None` when there is nothing to send.
///
/// A snapshot error is skipped rather than framed. The panel fetches once
/// itself when it mounts and shows that failure in its alert row, and the
/// realistic errors here are constant ones (`HOME` unset); turning a
/// transient read failure into a frame would flap an alert on and off
/// under the user instead of informing them.
fn tick_frame(app: &tauri::AppHandle, kind: Kind) -> Option<String> {
    let json = match kind {
        Kind::Usage => crate::usage::usage_snapshot(app.state())
            .ok()
            .map(|v| frame(kind, &v)),
        Kind::Hooks => crate::hooks::hooks_snapshot(app.state())
            .ok()
            .map(|v| frame(kind, &v)),
        Kind::HookLog => crate::hooks::hook_log_snapshot(app.state())
            .ok()
            .map(|v| frame(kind, &v)),
    };
    json?.ok()
}

/// Start (or replace) this window's panel stream and return its id.
///
/// Replacing rather than accumulating is deliberate: the webview mounts
/// at most one panel tab at a time (App.tsx), so its entire interest is
/// expressible as one set of kinds, and one owner means one thread.
/// Dropping the previous sender ends the previous thread — the same
/// replace-then-shut-down shape as `SessionState`.
///
/// Only the requested kinds are polled, which is what preserves the
/// property the `setInterval` version had for free: a hidden panel costs
/// nothing, because an unmounted panel's kind is not in the set.
#[tauri::command]
pub fn panel_stream(
    app: tauri::AppHandle,
    state: tauri::State<'_, PanelStreamState>,
    kinds: Vec<Kind>,
    chan: Channel<InvokeResponseBody>,
) -> Result<u64, String> {
    if kinds.is_empty() {
        return Err("panel_stream needs at least one kind".into());
    }
    let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
    let (stop_tx, stop_rx) = mpsc::channel::<()>();

    // A thread, not a tokio task: every kind here is blocking file I/O,
    // and `recv_timeout` gives the cadence and the shutdown wakeup in one
    // call — an explicit stop and a dropped sender both end the loop.
    std::thread::spawn(move || {
        let mut gate = ChangeGate::default();
        loop {
            for &kind in &kinds {
                let Some(payload) = tick_frame(&app, kind) else {
                    continue;
                };
                if !gate.accept(kind, &payload) {
                    continue;
                }
                // A send error means the webview is gone; so is the point
                // of polling for it.
                if chan.send(InvokeResponseBody::Json(payload)).is_err() {
                    return;
                }
            }
            match stop_rx.recv_timeout(TICK) {
                Err(RecvTimeoutError::Timeout) => {}
                _ => return,
            }
        }
    });

    // Assigning drops the previous sender, which stops the old thread.
    *state.0.lock().unwrap() = Some(Running { id, _stop: stop_tx });
    Ok(id)
}

/// Stop the stream with `id`, if it is still the current one.
#[tauri::command]
pub fn panel_stream_stop(state: tauri::State<'_, PanelStreamState>, id: u64) {
    let mut cur = state.0.lock().unwrap();
    if stop_applies(cur.as_ref().map(|r| r.id), id) {
        *cur = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Serialize)]
    struct Fixture {
        n: u32,
    }

    #[test]
    fn gate_accepts_a_payload_once() {
        let mut gate = ChangeGate::default();
        assert!(gate.accept(Kind::Usage, "{\"a\":1}"));
        assert!(!gate.accept(Kind::Usage, "{\"a\":1}"));
        assert!(gate.accept(Kind::Usage, "{\"a\":2}"));
        // Back to a payload seen before is still a change: the gate
        // remembers what was last SENT, not everything ever sent.
        assert!(gate.accept(Kind::Usage, "{\"a\":1}"));
    }

    #[test]
    fn gate_is_per_kind() {
        let mut gate = ChangeGate::default();
        assert!(gate.accept(Kind::Hooks, "same"));
        // Identical bytes under a different kind are a different panel's
        // data and must still go out.
        assert!(gate.accept(Kind::HookLog, "same"));
        assert!(gate.accept(Kind::Usage, "same"));
        assert!(!gate.accept(Kind::Hooks, "same"));
    }

    #[test]
    fn frame_names_its_kind() {
        assert_eq!(
            frame(Kind::HookLog, &Fixture { n: 7 }).unwrap(),
            r#"{"kind":"hook_log","data":{"n":7}}"#
        );
        assert_eq!(
            frame(Kind::Usage, &[1u32, 2]).unwrap(),
            r#"{"kind":"usage","data":[1,2]}"#
        );
    }

    #[test]
    fn kinds_are_named_on_the_wire_and_typos_are_rejected() {
        assert_eq!(
            serde_json::from_str::<Kind>("\"hook_log\"").unwrap(),
            Kind::HookLog
        );
        assert_eq!(
            serde_json::from_str::<Kind>("\"usage\"").unwrap(),
            Kind::Usage
        );
        assert!(serde_json::from_str::<Kind>("\"hookLog\"").is_err());
        assert!(serde_json::from_str::<Kind>("\"tokens\"").is_err());
    }

    #[test]
    fn a_stop_cannot_end_a_stream_it_does_not_name() {
        assert!(stop_applies(Some(4), 4));
        // The interleaving this exists for: stop(4) arrives after
        // start(5) replaced it.
        assert!(!stop_applies(Some(5), 4));
        assert!(!stop_applies(None, 4));
    }
}
