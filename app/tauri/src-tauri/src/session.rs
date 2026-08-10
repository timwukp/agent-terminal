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

    // Reader pump: daemon events → channel (tagged binary).
    tokio::spawn(async move {
        while let Some(ev) = rx.recv().await {
            match ev {
                Event::Output(bytes) => {
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

/// Terminal input. Takes the raw IPC body instead of a named `bytes`
/// argument: the webview sends an ArrayBuffer, which arrives as
/// `InvokeBody::Raw`, and a named arg cannot be deserialized from a
/// bytes payload at all (tauri::ipc::CommandItem::deserialize_json).
/// Raw also skips JSON-encoding every keystroke as a number array.
#[tauri::command]
pub async fn stdin_data(
    state: tauri::State<'_, SessionState>,
    request: tauri::ipc::Request<'_>,
) -> Result<(), String> {
    let bytes = match request.body() {
        tauri::ipc::InvokeBody::Raw(b) => b.as_slice(),
        tauri::ipc::InvokeBody::Json(_) => return Err("stdin_data expects a bytes payload".into()),
    };
    send_frame(&state, proto::stdin_data(bytes)).await
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
