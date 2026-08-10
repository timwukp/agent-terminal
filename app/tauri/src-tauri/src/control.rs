//! Control-plane commands: session listing, creation, kill. Each call
//! opens a short-lived connection — the daemon treats connections as
//! cheap (the CLI opens one per command), and a persistent control
//! connection would only add reconnect states to manage. The sidebar
//! polls list_sessions every 2 s (design: ux-spec.md); if that ever
//! measures as a problem, MSG_SESSION_EVENT push is the deferred fix.

use serde::Serialize;
use std::time::Duration;

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
