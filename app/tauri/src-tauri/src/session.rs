//! Tauri-side session bridge: owns the attach connection, pumps daemon
//! events into a binary IPC channel for the webview, accepts stdin and
//! control commands back.
//!
//! Transport: tauri::ipc::Channel with raw payloads — OUTPUT bytes reach
//! xterm.js as ArrayBuffer with no base64 tax and no extra listening
//! socket (a localhost port would contradict the daemon's 0700-socket
//! posture). Event framing over the channel is one leading tag byte so
//! the JS side can route without JSON-parsing terminal data:
//!   0x30 raw output bytes            (feed to xterm verbatim)
//!   0x31 snapshot: u16 cols, u16 rows LE, then blob (feed verbatim)
//!   0x4a JSON control event (layout, err, exits, closed)

use serde::Serialize;
use std::sync::Mutex;
use tauri::ipc::{Channel, InvokeResponseBody};
use tokio::sync::mpsc;

use crate::idle;
use at_client::{connect, Event};
use at_proto as proto;

/// Commands the UI can send to the live connection's writer task.
enum Cmd {
    Frame(Vec<u8>),
    Shutdown,
}

/// One attached session (at most one per window in v1).
pub struct Attachment {
    cmd_tx: mpsc::Sender<Cmd>,
}

#[derive(Default)]
pub struct SessionState(pub Mutex<Option<Attachment>>);

#[derive(Serialize, Clone)]
#[serde(tag = "kind", rename_all = "snake_case")]
enum CtrlEvent {
    Layout {
        view_cols: u16,
        view_rows: u16,
        active_id: u8,
        panes: Vec<PaneRectJs>,
    },
    Err {
        code: u16,
        msg: String,
    },
    SessionExited {
        exit_status: i32,
    },
    /// The attached session was demonstrably working (sustained output)
    /// and has now been silent long enough to call it finished. Emitted by
    /// the idle machine (src/idle.rs); the webview decides whether that
    /// becomes an OS notification — focus and mute state live there.
    TurnDone {},
    PaneExited {
        pane_id: u8,
        exit_status: i32,
    },
    Closed {
        error: Option<String>,
    },
}

#[derive(Serialize, Clone)]
struct PaneRectJs {
    id: u8,
    x: u16,
    y: u16,
    cols: u16,
    rows: u16,
}

fn send_ctrl(chan: &Channel<InvokeResponseBody>, ev: &CtrlEvent) {
    let mut buf = vec![0x4a];
    buf.extend_from_slice(
        serde_json::to_string(ev)
            .expect("ctrl event serializes")
            .as_bytes(),
    );
    let _ = chan.send(InvokeResponseBody::Raw(buf));
}

/// Attach to `session` and pump events into `chan` until the connection
/// closes or the attachment is replaced. Returns once attached (snapshot
/// will arrive over the channel).
#[tauri::command]
pub async fn attach_session(
    state: tauri::State<'_, SessionState>,
    session: String,
    cols: u16,
    rows: u16,
    chan: Channel<InvokeResponseBody>,
) -> Result<(), String> {
    let path = at_client::default_socket_path();
    let (mut client, mut rx) = connect(&path, proto::CLIENT_CAP_PANES)
        .await
        .map_err(|e| e.to_string())?;
    client
        .send(&proto::attach(cols, rows, 0, &session).map_err(|e| e.0.to_string())?)
        .await
        .map_err(|e| e.to_string())?;

    let (cmd_tx, mut cmd_rx) = mpsc::channel::<Cmd>(64);

    // Writer task: UI commands → socket.
    tokio::spawn(async move {
        while let Some(cmd) = cmd_rx.recv().await {
            match cmd {
                Cmd::Frame(f) => {
                    if client.send(&f).await.is_err() {
                        break;
                    }
                }
                Cmd::Shutdown => break,
            }
        }
        client.shutdown().await;
    });

    // Reader pump: daemon events → channel (tagged binary). Also hosts the
    // idle machine: this task sees every OUTPUT regardless of pane count,
    // and unlike a webview timer it is never throttled when the window is
    // occluded — which is exactly when a notification matters.
    tokio::spawn(async move {
        let mut machine = idle::Machine::new(idle::Config::default());
        loop {
            // Race the next event against the machine's deadline. No
            // deadline (IDLE) means nothing to time out — just wait.
            let ev = if let Some(deadline) = machine.next_deadline() {
                tokio::select! {
                    ev = rx.recv() => ev,
                    () = tokio::time::sleep_until(deadline.into()) => {
                        if machine.on_tick(std::time::Instant::now()) {
                            send_ctrl(&chan, &CtrlEvent::TurnDone {});
                        }
                        continue;
                    }
                }
            } else {
                rx.recv().await
            };
            let Some(ev) = ev else { return };
            match ev {
                Event::Output(bytes) => {
                    // Snapshots deliberately do NOT feed the machine: a
                    // snapshot is a repaint of existing state on attach,
                    // not the child producing anything.
                    machine.on_output(std::time::Instant::now());
                    let mut buf = Vec::with_capacity(1 + bytes.len());
                    buf.push(0x30);
                    buf.extend_from_slice(&bytes);
                    if chan.send(InvokeResponseBody::Raw(buf)).is_err() {
                        return;
                    }
                }
                Event::Snapshot {
                    cols, rows, blob, ..
                } => {
                    let mut buf = Vec::with_capacity(5 + blob.len());
                    buf.push(0x31);
                    buf.extend_from_slice(&cols.to_le_bytes());
                    buf.extend_from_slice(&rows.to_le_bytes());
                    buf.extend_from_slice(&blob);
                    if chan.send(InvokeResponseBody::Raw(buf)).is_err() {
                        return;
                    }
                }
                Event::Layout(l) => send_ctrl(
                    &chan,
                    &CtrlEvent::Layout {
                        view_cols: l.view_cols,
                        view_rows: l.view_rows,
                        active_id: l.active_id,
                        panes: l
                            .panes
                            .iter()
                            .map(|p| PaneRectJs {
                                id: p.id,
                                x: p.x,
                                y: p.y,
                                cols: p.cols,
                                rows: p.rows,
                            })
                            .collect(),
                    },
                ),
                Event::Err { code, msg } => send_ctrl(&chan, &CtrlEvent::Err { code, msg }),
                Event::SessionExited(exit_status) => {
                    send_ctrl(&chan, &CtrlEvent::SessionExited { exit_status })
                }
                Event::PaneExited {
                    pane_id,
                    exit_status,
                } => send_ctrl(
                    &chan,
                    &CtrlEvent::PaneExited {
                        pane_id,
                        exit_status,
                    },
                ),
                Event::Closed(error) => {
                    send_ctrl(&chan, &CtrlEvent::Closed { error });
                    return;
                }
                // Sidebar traffic (session lists) rides the control
                // connection, not the attach one; scrollback/pong unused
                // in v1 rendering.
                Event::SessionList(_) | Event::Scrollback { .. } | Event::Pong(_) => {}
            }
        }
    });

    // Replacing a previous attachment shuts it down (session switch).
    let old = state.0.lock().unwrap().replace(Attachment { cmd_tx });
    if let Some(old) = old {
        let _ = old.cmd_tx.try_send(Cmd::Shutdown);
    }
    Ok(())
}

/// Queue one frame for the writer task.
///
/// Awaits a full queue rather than `try_send`ing: a dropped frame here is
/// a keystroke the user typed and never saw, and a paste or a fast typist
/// can outrun a busy writer. The clone releases the mutex before the
/// await — a std Mutex must not be held across one.
async fn send_frame(state: &tauri::State<'_, SessionState>, frame: Vec<u8>) -> Result<(), String> {
    let tx = {
        let guard = state.0.lock().unwrap();
        guard.as_ref().ok_or("not attached")?.cmd_tx.clone()
    };
    tx.send(Cmd::Frame(frame)).await.map_err(|e| e.to_string())
}

/// Decode a keystroke payload from whichever IPC body shape arrived.
///
/// Two shapes are legitimate, because Tauri picks the transport, not us:
///
/// * `Raw` — the custom-protocol path (`ipc://localhost/...` POST). The
///   ArrayBuffer arrives verbatim; this is the fast path.
/// * `Json` array of byte-sized numbers — the `postMessage` fallback.
///   `sendIpcMessage` re-serializes the whole envelope, and its
///   `JSON.stringify` replacer turns a `Uint8Array` into `Array.from(val)`
///   (tauri-2.11.5/scripts/process-ipc-message-fn.js:26), so raw bytes
///   *cannot* survive that path. Measured: with `connect-src` missing the
///   `ipc:` scheme, the custom protocol fetch is CSP-blocked, Tauri falls
///   back, and every single keystroke arrives here as JSON.
///
/// The fallback is accepted rather than rejected on purpose. It costs
/// throughput, not correctness — whereas refusing it makes the keyboard
/// completely dead, which is what a real UAT run produced.
fn decode_stdin(body: &tauri::ipc::InvokeBody) -> Result<Vec<u8>, String> {
    match body {
        tauri::ipc::InvokeBody::Raw(b) => Ok(b.clone()),
        tauri::ipc::InvokeBody::Json(serde_json::Value::Array(items)) => items
            .iter()
            .map(|v| {
                v.as_u64()
                    .and_then(|n| u8::try_from(n).ok())
                    .ok_or_else(|| format!("stdin payload has a non-byte element: {v}"))
            })
            .collect(),
        // Anything else is a frontend/backend mismatch rather than a
        // transport choice, so name the shape instead of guessing a cause.
        tauri::ipc::InvokeBody::Json(v) => Err(format!(
            "stdin_data needs a byte array or a bytes payload, got {v}"
        )),
    }
}

/// Terminal input. Takes the raw IPC body instead of a named `bytes`
/// argument: the webview sends an ArrayBuffer, which arrives as
/// `InvokeBody::Raw`, and a named arg cannot be deserialized from a
/// bytes payload at all (tauri::ipc::CommandItem::deserialize_json).
#[tauri::command]
pub async fn stdin_data(
    state: tauri::State<'_, SessionState>,
    request: tauri::ipc::Request<'_>,
) -> Result<(), String> {
    let bytes = decode_stdin(request.body())?;
    send_frame(&state, proto::stdin_data(&bytes)).await
}

#[cfg(test)]
mod tests {
    use super::*;
    use tauri::ipc::InvokeBody;

    #[test]
    fn raw_body_is_the_fast_path() {
        let body = InvokeBody::Raw(vec![0x1b, 0x5b, 0x41]);
        assert_eq!(decode_stdin(&body).unwrap(), vec![0x1b, 0x5b, 0x41]);
    }

    #[test]
    fn postmessage_fallback_array_is_accepted() {
        // The shape Tauri's postMessage path produces for a Uint8Array.
        // Rejecting this is what made the keyboard dead in UAT: the
        // custom-protocol fetch was CSP-blocked, so EVERY keystroke came
        // through here.
        let body = InvokeBody::Json(serde_json::json!([27, 91, 73]));
        assert_eq!(decode_stdin(&body).unwrap(), vec![27, 91, 73]);
    }

    #[test]
    fn empty_array_decodes_to_no_bytes() {
        let body = InvokeBody::Json(serde_json::json!([]));
        assert_eq!(decode_stdin(&body).unwrap(), Vec::<u8>::new());
    }

    #[test]
    fn out_of_range_and_negative_elements_are_rejected() {
        // A byte array is the contract; 256 and -1 mean the sender is not
        // encoding bytes, which must not be silently truncated to u8.
        for v in [serde_json::json!([256]), serde_json::json!([-1])] {
            let err = decode_stdin(&InvokeBody::Json(v)).unwrap_err();
            assert!(err.contains("non-byte"), "unexpected error: {err}");
        }
    }

    /// The CSP is load-bearing, not decoration. On macOS the IPC fetch
    /// targets `ipc://localhost/<cmd>` (tauri-2.11.5/scripts/core.js
    /// convertFileSrc) while the page is served from `tauri://localhost`,
    /// so a policy without the `ipc:` scheme blocks it, Tauri falls back
    /// to postMessage, and raw byte payloads stop being possible.
    /// Mutation-verified by hand: deleting `ipc:` and rebuilding
    /// reproduced "IPC custom protocol failed" plus a JSON body for every
    /// keystroke; restoring it produced `Raw` bodies again.
    #[test]
    fn csp_permits_the_ipc_scheme_for_the_custom_protocol() {
        let conf = include_str!("../tauri.conf.json");
        let v: serde_json::Value = serde_json::from_str(conf).expect("tauri.conf.json is valid");
        let csp = v["app"]["security"]["csp"]
            .as_str()
            .expect("csp is a string");
        let connect = csp
            .split(';')
            .map(str::trim)
            .find(|d| d.starts_with("connect-src"))
            .unwrap_or_else(|| panic!("no connect-src directive in csp: {csp}"));
        assert!(
            connect.split_whitespace().any(|s| s == "ipc:"),
            "connect-src must allow the ipc: scheme, got: {connect}"
        );
    }

    /// Bundling is config + files that only meet at `tauri build` time,
    /// which CI does not run (a release bundle per PR is minutes of
    /// build). This pins the meeting point: every icon the config names
    /// exists, and the bundle stays active — so a renamed icon or a
    /// "temporarily" disabled bundle reds the unit suite instead of the
    /// next release attempt.
    #[test]
    fn bundle_config_names_icons_that_exist() {
        let conf = include_str!("../tauri.conf.json");
        let v: serde_json::Value = serde_json::from_str(conf).expect("tauri.conf.json is valid");
        assert_eq!(
            v["bundle"]["active"],
            serde_json::json!(true),
            "bundling is how the .app (and with it, real macOS notifications) exists"
        );
        let icons = v["bundle"]["icon"]
            .as_array()
            .expect("bundle.icon is a list");
        assert!(!icons.is_empty(), "an empty icon list fails `tauri build`");
        let manifest = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
        for icon in icons {
            let rel = icon.as_str().expect("icon entries are paths");
            assert!(
                manifest.join(rel).is_file(),
                "config names a missing icon: {rel}"
            );
        }
    }

    #[test]
    fn non_array_json_names_the_shape_it_got() {
        let err = decode_stdin(&InvokeBody::Json(serde_json::json!({"bytes": [1]}))).unwrap_err();
        assert!(err.contains("byte array"), "unexpected error: {err}");
        // Must NOT blame a stale build: the real cause of a JSON body was
        // a CSP that blocked the custom protocol, and the stale-frontend
        // wording sent a UAT session chasing a rebuild that changed nothing.
        assert!(!err.contains("stale"), "misleading cause in: {err}");
    }
}

#[tauri::command]
pub async fn resize(
    state: tauri::State<'_, SessionState>,
    cols: u16,
    rows: u16,
) -> Result<(), String> {
    send_frame(&state, proto::resize(cols, rows)).await
}

#[tauri::command]
pub async fn select_pane(state: tauri::State<'_, SessionState>, pane_id: u8) -> Result<(), String> {
    send_frame(&state, proto::select_pane(proto::SelectMode::ById, pane_id)).await
}

#[tauri::command]
pub async fn zoom_toggle(state: tauri::State<'_, SessionState>) -> Result<(), String> {
    send_frame(&state, proto::select_pane(proto::SelectMode::ZoomToggle, 0)).await
}

#[tauri::command]
pub async fn split_pane(
    state: tauri::State<'_, SessionState>,
    stacked: bool,
) -> Result<(), String> {
    send_frame(&state, proto::split_pane(stacked, proto::PANE_ACTIVE)).await
}

#[tauri::command]
pub async fn close_pane(state: tauri::State<'_, SessionState>) -> Result<(), String> {
    send_frame(&state, proto::close_pane(proto::PANE_ACTIVE)).await
}

/// Detach from the current session (drop the connection; the session
/// keeps running daemon-side).
#[tauri::command]
pub fn detach(state: tauri::State<'_, SessionState>) -> Result<(), String> {
    let old = state.0.lock().unwrap().take();
    if let Some(old) = old {
        let _ = old.cmd_tx.try_send(Cmd::Shutdown);
    }
    Ok(())
}
