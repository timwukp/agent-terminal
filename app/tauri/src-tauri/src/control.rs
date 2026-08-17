//! Control-plane commands: session listing, creation, kill. Each call
//! opens a short-lived connection — the daemon treats connections as
//! cheap (the CLI opens one per command), and a persistent control
//! connection would only add reconnect states to manage.
//!
//! `session_watch` is the one exception, and it earns the reconnect
//! states: it holds a connection open purely to receive
//! MSG_SESSIONS_CHANGED, which is what lets the sidebar stop polling
//! `list_sessions` every 2 s. Listing itself stays a roundtrip — the push
//! carries no data, so the answer to "what changed" is still one LIST2.
//!
//! What the watcher does NOT do is detect a daemon that is alive but
//! wedged: with push, that reads as "nothing changed" where the poll used
//! to surface a 5 s timeout in the sidebar. Accepted deliberately — the
//! attach connection (session.rs) has the same property, and a wedged
//! daemon shows up as a frozen terminal, which is a far louder signal
//! than a sidebar row. Every failure the OS actually reports (daemon exit,
//! `reload` replacing it, socket gone) arrives as EOF and puts the sidebar
//! back on the poll within one frame.

use serde::Serialize;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;
use std::time::Duration;

use tauri::ipc::{Channel, InvokeResponseBody};
use tokio::sync::mpsc;

use at_client::{connect, Event};
use at_proto as proto;

/// One sidebar row (SESSION_LIST2 entry, GUI-shaped).
#[derive(Serialize, Clone)]
pub struct SessionRow {
    pub name: String,
    pub view_cols: u16,
    pub view_rows: u16,
    pub alive: bool,
    pub nclients: u8,
    pub pid: u32,
    /// None when an old daemon answered the v1 fallback.
    pub npanes: Option<u8>,
    pub zoomed: Option<bool>,
}

/// Bound every control roundtrip: a wedged daemon fails the call, the
/// UI shows the error — never a spinner forever (attach.c's 5 s rule).
const CONTROL_TIMEOUT: Duration = Duration::from_secs(5);

async fn control_roundtrip(
    request: Vec<u8>,
    mut accept: impl FnMut(Event) -> Option<Result<Vec<SessionRow>, String>>,
) -> Result<Vec<SessionRow>, String> {
    let path = at_client::default_socket_path();
    let (mut client, mut rx) = connect(&path, 0).await.map_err(|e| e.to_string())?;
    client.send(&request).await.map_err(|e| e.to_string())?;
    let result = tokio::time::timeout(CONTROL_TIMEOUT, async {
        while let Some(ev) = rx.recv().await {
            if let Some(r) = accept(ev) {
                return r;
            }
        }
        Err("connection closed before the daemon answered".into())
    })
    .await
    .map_err(|_| "no answer from daemon within 5s".to_string())?;
    client.shutdown().await;
    result
}

fn rows_from(entries: Vec<proto::SessionEntry>) -> Vec<SessionRow> {
    entries
        .into_iter()
        .map(|e| SessionRow {
            name: e.name,
            view_cols: e.view_cols,
            view_rows: e.view_rows,
            alive: e.alive,
            nclients: e.nclients,
            pid: e.pid,
            npanes: e.npanes,
            zoomed: e.zoomed,
        })
        .collect()
}

/// List sessions (LIST2; the at-client event layer already falls back
/// to the v1 answer shape if an old daemon skips the request).
#[tauri::command]
pub async fn list_sessions() -> Result<Vec<SessionRow>, String> {
    control_roundtrip(proto::list_sessions2(), |ev| match ev {
        Event::SessionList(l) => Some(Ok(rows_from(l))),
        Event::Err { code, msg } => Some(Err(format!("daemon error {code}: {msg}"))),
        Event::Closed(e) => Some(Err(e.unwrap_or_else(|| "connection closed".into()))),
        _ => None,
    })
    .await
}

/// Create a detached session (the GUI attaches separately — same as the
/// scripted `new < /dev/null` contract: daemon confirms, session
/// persists with zero clients). The confirmation is the SNAPSHOT the
/// creating connection receives; dropping the connection right after
/// leaves the session running.
#[tauri::command]
pub async fn new_session(
    name: String,
    argv: Vec<String>,
    cols: u16,
    rows: u16,
) -> Result<(), String> {
    let argv_refs: Vec<&str> = argv.iter().map(|s| s.as_str()).collect();
    let frame = proto::new_session(cols, rows, &name, &argv_refs).map_err(|e| e.0.to_string())?;
    control_roundtrip(frame, |ev| match ev {
        Event::Snapshot { .. } => Some(Ok(Vec::new())),
        Event::Err { code, msg } => Some(Err(format!("daemon error {code}: {msg}"))),
        Event::Closed(e) => Some(Err(e.unwrap_or_else(|| "connection closed".into()))),
        _ => None,
    })
    .await
    .map(|_| ())
}

/// Kill a session. The daemon answers with a fresh session list (that
/// is the CLI's contract too — client/main.c cmd_kill), or ERR.
#[tauri::command]
pub async fn kill_session(name: String) -> Result<(), String> {
    let frame = proto::kill_session(&name).map_err(|e| e.0.to_string())?;
    control_roundtrip(frame, |ev| match ev {
        Event::SessionList(_) => Some(Ok(Vec::new())),
        Event::Err { code, msg } => Some(Err(format!("daemon error {code}: {msg}"))),
        Event::Closed(e) => Some(Err(e.unwrap_or_else(|| "connection closed".into()))),
        _ => None,
    })
    .await
    .map(|_| ())
}

// ---- session-table push (MSG_SESSIONS_CHANGED) ----

/// Frames the sidebar routes on, over its own channel.
///
/// Two messages rather than one, because they answer different questions.
/// `sessions_changed` says "re-list now"; `sessions_push` says "you can
/// stop polling" (or "start again"). Collapsing them into a single
/// "changed" frame would make the fallback undecidable: no frames is
/// exactly what a working, idle daemon looks like.
#[derive(Serialize)]
#[serde(tag = "kind", content = "data", rename_all = "snake_case")]
enum WatchFrame {
    /// Is push live? A struct variant, not a bare bool, so a later reason
    /// code is an additive field instead of a breaking reshape.
    SessionsPush { live: bool },
    /// The session table changed. Carries nothing — the daemon's message
    /// carries nothing (proto.h 0x39).
    SessionsChanged,
}

/// How the last watch attempt ended. The delay is derived from this
/// rather than from a retry counter: each ending has a different cause
/// and therefore a different right answer, and a counter would blur them
/// into one curve.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Attempt {
    /// A live push connection ended — daemon exited, or `reload` replaced
    /// it (which disconnects every client by design).
    Dropped,
    /// connect() or the handshake failed: usually no daemon yet.
    Unreachable,
    /// The daemon answered but does not advertise
    /// `SERVER_CAP_SESSION_EVENTS` — it is older than this message.
    Incapable,
}

/// A drop proves a daemon existed a moment ago, and the reload path
/// re-binds in ~100 ms; the window this delay opens is a window where the
/// user is looking at a stale session list.
const RECONNECT_DELAY: Duration = Duration::from_secs(1);
/// No daemon at all is the steady state of a freshly opened window, and
/// the fallback poll (2 s) is running meanwhile. Retrying at the poll's
/// own cadence keeps the floor honest: turning the poll off must not make
/// this side knock harder than the poll it replaced.
const UNREACHABLE_DELAY: Duration = Duration::from_secs(2);
/// An old daemon becomes a new one only via `make install` + reload, so
/// re-probing is about not requiring an app restart after an upgrade —
/// 30 s bounds that at a cost of one connect per half minute.
const PROBE_DELAY: Duration = Duration::from_secs(30);

pub fn retry_after(attempt: Attempt) -> Duration {
    match attempt {
        Attempt::Dropped => RECONNECT_DELAY,
        Attempt::Unreachable => UNREACHABLE_DELAY,
        Attempt::Incapable => PROBE_DELAY,
    }
}

/// Does this daemon's HELLO_OK promise MSG_SESSIONS_CHANGED?
///
/// `None` means the daemon sent no server_flags field at all, which by
/// definition predates the message — so absence must read as "no", never
/// as "assume yes and wait". Waiting on a daemon that will never send is
/// indistinguishable from an idle one, and the sidebar would simply stop
/// updating.
pub fn daemon_pushes_session_events(server_flags: Option<u16>) -> bool {
    server_flags.is_some_and(|f| f & proto::SERVER_CAP_SESSION_EVENTS != 0)
}

/// Remembers the push state last sent, so the sidebar hears about
/// transitions and not about every reconnect attempt.
#[derive(Default)]
struct LiveGate {
    last: Option<bool>,
}

impl LiveGate {
    /// True — and remembers `live` — when it differs from the last state
    /// sent. The first call always reports true: the frontend's default is
    /// an assumption, and an assumption is exactly what this frame exists
    /// to replace.
    fn accept(&mut self, live: bool) -> bool {
        if self.last == Some(live) {
            return false;
        }
        self.last = Some(live);
        true
    }
}

/// Send one frame; `None` means the webview is gone (end the watcher).
fn send(chan: &Channel<InvokeResponseBody>, f: &WatchFrame) -> Option<()> {
    let json = serde_json::to_string(f).expect("watch frame serializes");
    chan.send(InvokeResponseBody::Json(json)).ok()
}

fn send_live(chan: &Channel<InvokeResponseBody>, gate: &mut LiveGate, live: bool) -> Option<()> {
    if !gate.accept(live) {
        return Some(());
    }
    send(chan, &WatchFrame::SessionsPush { live })
}

/// One connection's lifetime: connect, check the capability, forward every
/// notification until the connection ends. `None` means the webview is
/// gone; otherwise the ending that decides the retry delay.
async fn watch_once(chan: &Channel<InvokeResponseBody>, gate: &mut LiveGate) -> Option<Attempt> {
    let path = at_client::default_socket_path();
    let (client, mut rx) = match connect(&path, proto::CLIENT_CAP_SESSION_EVENTS).await {
        Ok(c) => c,
        Err(_) => {
            send_live(chan, gate, false)?;
            return Some(Attempt::Unreachable);
        }
    };
    if !daemon_pushes_session_events(client.hello.server_flags) {
        send_live(chan, gate, false)?;
        client.shutdown().await;
        return Some(Attempt::Incapable);
    }
    // `client` is held for the whole loop on purpose: dropping it closes
    // the write half, the daemon reads 0 and disconnects us (server.c
    // treats EOF as a departed client), so a "read-only" connection that
    // dropped its writer would die immediately.
    send_live(chan, gate, true)?;
    while let Some(ev) = rx.recv().await {
        match ev {
            Event::SessionsChanged => send(chan, &WatchFrame::SessionsChanged)?,
            Event::Closed(_) => break,
            // A CAP_SESSION_EVENTS-only connection is attached to nothing,
            // so nothing else is expected here — and an unexpected frame
            // is not a reason to give up a working notification stream.
            _ => {}
        }
    }
    send_live(chan, gate, false)?;
    client.shutdown().await;
    Some(Attempt::Dropped)
}

/// The watcher this window is running. Dropping `_stop` ends its task.
pub struct WatchRunning {
    id: u64,
    _stop: mpsc::Sender<()>,
}

#[derive(Default)]
pub struct SessionWatchState(pub Mutex<Option<WatchRunning>>);

static NEXT_WATCH_ID: AtomicU64 = AtomicU64::new(1);

/// Start (or replace) this window's session-table watcher; returns its id.
///
/// Returns as soon as the task is spawned rather than after the first
/// connect: the sidebar must render (and poll) immediately, and whether
/// push is available arrives as a `sessions_push` frame precisely so this
/// call never has to wait to find out.
#[tauri::command]
pub async fn session_watch(
    state: tauri::State<'_, SessionWatchState>,
    chan: Channel<InvokeResponseBody>,
) -> Result<u64, String> {
    let id = NEXT_WATCH_ID.fetch_add(1, Ordering::Relaxed);
    let (stop_tx, mut stop_rx) = mpsc::channel::<()>(1);

    tokio::spawn(async move {
        let mut gate = LiveGate::default();
        loop {
            // Both stop paths are one branch: an explicit stop sends, and
            // a replaced watcher drops the sender, which makes recv()
            // return None.
            let attempt = tokio::select! {
                _ = stop_rx.recv() => return,
                a = watch_once(&chan, &mut gate) => a,
            };
            let Some(attempt) = attempt else { return };
            tokio::select! {
                _ = stop_rx.recv() => return,
                () = tokio::time::sleep(retry_after(attempt)) => {}
            }
        }
    });

    // Assigning drops the previous sender, which stops the old task.
    *state.0.lock().unwrap() = Some(WatchRunning { id, _stop: stop_tx });
    Ok(id)
}

/// Stop the watcher with `id`, if it is still the current one. Same
/// StrictMode-safe rule as the panel stream — see panels::stop_applies.
#[tauri::command]
pub fn session_watch_stop(state: tauri::State<'_, SessionWatchState>, id: u64) {
    let mut cur = state.0.lock().unwrap();
    if crate::panels::stop_applies(cur.as_ref().map(|r| r.id), id) {
        *cur = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Pinned as bytes: the webview matches on `kind` and reads
    /// `data.live` (src/sidebar/api.ts), so a rename here is a silently
    /// ignored frame there, not a type error.
    #[test]
    fn watch_frames_are_pinned() {
        let live = serde_json::to_string(&WatchFrame::SessionsPush { live: true }).unwrap();
        assert_eq!(live, r#"{"kind":"sessions_push","data":{"live":true}}"#);
        let down = serde_json::to_string(&WatchFrame::SessionsPush { live: false }).unwrap();
        assert_eq!(down, r#"{"kind":"sessions_push","data":{"live":false}}"#);
        // No `data` key at all — the message it mirrors has no payload.
        let changed = serde_json::to_string(&WatchFrame::SessionsChanged).unwrap();
        assert_eq!(changed, r#"{"kind":"sessions_changed"}"#);
    }

    #[test]
    fn a_daemon_without_the_capability_bit_is_not_trusted_to_push() {
        assert!(!daemon_pushes_session_events(None), "pre-flags daemon");
        assert!(!daemon_pushes_session_events(Some(0)));
        // The bit that is NOT ours: a panes-capable daemon that predates
        // 0x39 advertises 0x0001, and reading any nonzero flags as "yes"
        // would leave the sidebar waiting forever on it.
        assert!(!daemon_pushes_session_events(Some(proto::SERVER_CAP_PANES)));
        assert!(daemon_pushes_session_events(Some(
            proto::SERVER_CAP_SESSION_EVENTS
        )));
        assert!(daemon_pushes_session_events(Some(
            proto::SERVER_CAP_PANES | proto::SERVER_CAP_SESSION_EVENTS
        )));
        // Unknown future bits alongside ours must not mask it.
        assert!(daemon_pushes_session_events(Some(0xffff)));
    }

    #[test]
    fn the_live_gate_reports_transitions_only() {
        let mut gate = LiveGate::default();
        // First observation always goes out, even the pessimistic one.
        assert!(gate.accept(false));
        assert!(!gate.accept(false), "a second failed connect is not news");
        assert!(gate.accept(true));
        assert!(!gate.accept(true));
        assert!(gate.accept(false), "losing push must reach the sidebar");
    }

    /// The floor that keeps this from being a worse poll than the poll it
    /// replaces: while no daemon is reachable the sidebar is still polling
    /// at 2 s, so knocking faster than that would add load in exactly the
    /// state where nothing is being saved.
    #[test]
    fn no_retry_is_faster_than_the_poll_it_replaces() {
        assert!(retry_after(Attempt::Unreachable) >= Duration::from_secs(2));
        // A drop is different: a daemon demonstrably existed, and `reload`
        // re-binds in ~100 ms, so this one is allowed to be quick.
        assert!(retry_after(Attempt::Dropped) <= Duration::from_secs(1));
        // An old daemon changes version only via install+reload; probing
        // it at the drop cadence would be 30x the connects for nothing.
        assert!(retry_after(Attempt::Incapable) >= Duration::from_secs(30));
    }
}
