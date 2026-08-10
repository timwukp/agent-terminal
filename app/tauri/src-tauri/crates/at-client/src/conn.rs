//! One daemon connection: HELLO handshake, then a reader task that
//! parses frames into typed events on a bounded channel, plus a writer
//! handle for client→daemon messages.
//!
//! Error discipline mirrors the C client (attach.c): an ERR before
//! attach succeeds is fatal; after that it is advisory — surfaced as an
//! event, connection kept — except ERR_VERSION. Framing violations
//! (oversize) kill the connection: at-proto's Decoder poisons itself for
//! the same reason proto_read_frame returns -1.

use std::path::Path;
use std::time::Duration;

use at_proto as proto;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;
use tokio::sync::mpsc;

/// Handshake bound. The C client gives a wedged daemon 5 s to confirm
/// anything before failing the command; same figure here.
pub const HELLO_TIMEOUT: Duration = Duration::from_secs(5);

/// Bounded event queue: when the consumer lags this far behind, the
/// reader stops reading the socket (backpressure to the daemon's write
/// buffer) instead of buffering without limit.
const EVENT_QUEUE: usize = 256;

#[derive(Debug)]
pub enum ClientError {
    /// Connect/handshake/socket io failed.
    Io(std::io::Error),
    /// The daemon violated framing (oversized frame) — connection dead.
    Protocol(&'static str),
    /// Daemon answered HELLO with something other than HELLO_OK, or an
    /// unsupported version.
    Handshake(String),
    /// HELLO_OK did not arrive within HELLO_TIMEOUT.
    Timeout,
}

impl std::fmt::Display for ClientError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ClientError::Io(e) => write!(f, "io: {e}"),
            ClientError::Protocol(m) => write!(f, "protocol violation: {m}"),
            ClientError::Handshake(m) => write!(f, "handshake: {m}"),
            ClientError::Timeout => write!(f, "no HELLO_OK from daemon within 5s"),
        }
    }
}

impl std::error::Error for ClientError {}

impl From<std::io::Error> for ClientError {
    fn from(e: std::io::Error) -> Self {
        ClientError::Io(e)
    }
}

/// Typed daemon→client events, in arrival order. Output/Snapshot carry
/// raw bytes for the terminal widget verbatim.
#[derive(Debug)]
pub enum Event {
    /// MSG_OUTPUT: feed to the terminal as-is.
    Output(Vec<u8>),
    /// MSG_SNAPSHOT: dims + sb_lines + the repaint blob (fed as-is).
    Snapshot {
        cols: u16,
        rows: u16,
        sb_lines: u64,
        blob: Vec<u8>,
    },
    /// MSG_LAYOUT (only when HELLO carried CLIENT_CAP_PANES).
    Layout(proto::Layout),
    /// MSG_SESSION_LIST2 / v1 fallback both surface here.
    SessionList(Vec<proto::SessionEntry>),
    /// MSG_ERR after attach: advisory unless code == ERR_VERSION.
    Err { code: u16, msg: String },
    /// MSG_SESSION_EXITED.
    SessionExited(i32),
    /// MSG_PANE_EXITED.
    PaneExited { pane_id: u8, exit_status: i32 },
    /// MSG_SCROLLBACK_DATA.
    Scrollback { first_seq: u64, lines: Vec<Vec<u8>> },
    /// MSG_PONG.
    Pong(u64),
    /// The connection ended: clean EOF (None) or an error (Some).
    /// Always the final event.
    Closed(Option<String>),
}

pub type EventRx = mpsc::Receiver<Event>;

/// A live connection. Dropping it closes the socket (reader task ends,
/// emits Closed).
pub struct Client {
    write_half: tokio::net::unix::OwnedWriteHalf,
    pub hello: proto::HelloOk,
    reader: tokio::task::JoinHandle<()>,
}

/// Connect, HELLO with `flags`, await HELLO_OK (bounded), then spawn the
/// reader. Returns the client (write side + daemon identity) and the
/// event stream.
pub async fn connect(path: &Path, flags: u16) -> Result<(Client, EventRx), ClientError> {
    let mut stream = UnixStream::connect(path).await?;
    stream.write_all(&proto::hello(flags)).await?;

    // Read frames until HELLO_OK; the daemon sends nothing else first,
    // but the loop keeps the decoder honest about partial reads.
    let mut dec = proto::Decoder::new();
    let hello = tokio::time::timeout(HELLO_TIMEOUT, async {
        let mut buf = [0u8; 4096];
        loop {
            if let Some(frame) = dec
                .next_frame()
                .map_err(|_| ClientError::Protocol("oversized frame in handshake"))?
            {
                if frame.msg_type == proto::MsgType::HelloOk as u8 {
                    return proto::parse_hello_ok(&frame.payload)
                        .map_err(|e| ClientError::Handshake(e.0.into()));
                }
                if frame.msg_type == proto::MsgType::Err as u8 {
                    let e = proto::parse_err(&frame.payload)
                        .map_err(|e| ClientError::Handshake(e.0.into()))?;
                    return Err(ClientError::Handshake(format!(
                        "daemon refused hello: {} (code {})",
                        e.msg, e.code
                    )));
                }
                return Err(ClientError::Handshake(format!(
                    "expected HELLO_OK first, got type 0x{:02x}",
                    frame.msg_type
                )));
            }
            let n = stream.read(&mut buf).await?;
            if n == 0 {
                return Err(ClientError::Handshake("daemon closed during hello".into()));
            }
            dec.feed(&buf[..n]);
        }
    })
    .await
    .map_err(|_| ClientError::Timeout)??;

    if hello.ver != proto::PROTO_VERSION {
        return Err(ClientError::Handshake(format!(
            "daemon speaks protocol v{}, client v{}",
            hello.ver,
            proto::PROTO_VERSION
        )));
    }

    let (read_half, write_half) = stream.into_split();
    let (tx, rx) = mpsc::channel(EVENT_QUEUE);
    let reader = tokio::spawn(read_loop(read_half, dec, tx));
    Ok((
        Client {
            write_half,
            hello,
            reader,
        },
        rx,
    ))
}

impl Client {
    /// Write one pre-encoded frame (at-proto encoder output).
    pub async fn send(&mut self, frame: &[u8]) -> Result<(), ClientError> {
        self.write_half.write_all(frame).await?;
        Ok(())
    }

    /// Abort the reader and close. Dropping does the same implicitly;
    /// this exists for explicit teardown in tests.
    pub async fn shutdown(self) {
        drop(self.write_half);
        self.reader.abort();
        let _ = self.reader.await;
    }
}

async fn read_loop(
    mut read_half: tokio::net::unix::OwnedReadHalf,
    mut dec: proto::Decoder,
    tx: mpsc::Sender<Event>,
) {
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        // Drain complete frames before reading more: backpressure lives
        // in tx.send (bounded), and a full queue must stop socket reads.
        loop {
            match dec.next_frame() {
                Ok(Some(frame)) => {
                    if let Some(ev) = frame_to_event(frame.msg_type, frame.payload) {
                        if tx.send(ev).await.is_err() {
                            return; // consumer gone; nothing to report to
                        }
                    }
                }
                Ok(None) => break,
                Err(_) => {
                    let _ = tx
                        .send(Event::Closed(Some(
                            "protocol violation: oversized frame".into(),
                        )))
                        .await;
                    return;
                }
            }
        }
        match read_half.read(&mut buf).await {
            Ok(0) => {
                let _ = tx.send(Event::Closed(None)).await;
                return;
            }
            Ok(n) => dec.feed(&buf[..n]),
            Err(e) => {
                let _ = tx.send(Event::Closed(Some(e.to_string()))).await;
                return;
            }
        }
    }
}

/// Map one frame to an event. Unknown types return None — skipped by
/// design (proto.h: framing makes that safe). Malformed known types are
/// also skipped rather than fatal: the C client tolerates them the same
/// way, and a wire bug in one message must not kill a live session view.
fn frame_to_event(msg_type: u8, payload: Vec<u8>) -> Option<Event> {
    use proto::MsgType as T;
    match msg_type {
        t if t == T::Output as u8 => Some(Event::Output(payload)),
        t if t == T::Snapshot as u8 => {
            let s = proto::parse_snapshot(&payload).ok()?;
            Some(Event::Snapshot {
                cols: s.cols,
                rows: s.rows,
                sb_lines: s.sb_lines,
                blob: s.blob.to_vec(),
            })
        }
        t if t == T::Layout as u8 => proto::parse_layout(&payload).ok().map(Event::Layout),
        t if t == T::SessionList2 as u8 => proto::parse_session_list2(&payload)
            .ok()
            .map(Event::SessionList),
        t if t == T::SessionList as u8 => proto::parse_session_list(&payload)
            .ok()
            .map(Event::SessionList),
        t if t == T::Err as u8 => {
            let e = proto::parse_err(&payload).ok()?;
            Some(Event::Err {
                code: e.code,
                msg: e.msg,
            })
        }
        t if t == T::SessionExited as u8 => proto::parse_session_exited(&payload)
            .ok()
            .map(Event::SessionExited),
        t if t == T::PaneExited as u8 => {
            proto::parse_pane_exited(&payload)
                .ok()
                .map(|(pane_id, exit_status)| Event::PaneExited {
                    pane_id,
                    exit_status,
                })
        }
        t if t == T::ScrollbackData as u8 => {
            proto::parse_scrollback_data(&payload)
                .ok()
                .map(|d| Event::Scrollback {
                    first_seq: d.first_seq,
                    lines: d.lines,
                })
        }
        t if t == T::Pong as u8 => proto::parse_pong(&payload).ok().map(Event::Pong),
        _ => None,
    }
}
