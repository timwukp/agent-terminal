//! Message-level encode (client→daemon) and parse (daemon→client).
//!
//! Every layout is verified against the daemon's serialization code, not
//! just proto.h's comments; each item cites both. Parsers implement the
//! protocol's evolution rules: unknown trailing payload bytes are
//! ignored, tail-optional fields parse only when present, SESSION_LIST2
//! entries are skipped by their length prefix.

use crate::encode_frame;

/// proto.h `enum proto_type`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum MsgType {
    Hello = 0x01,
    HelloOk = 0x02,
    Err = 0x03,
    ListSessions = 0x10,
    SessionList = 0x11,
    NewSession = 0x12,
    KillSession = 0x13,
    Attach = 0x14,
    Detach = 0x15,
    SplitPane = 0x16,
    ClosePane = 0x17,
    SelectPane = 0x18,
    Reload = 0x19,
    ListSessions2 = 0x1a,
    StdinData = 0x20,
    Resize = 0x21,
    Output = 0x30,
    Snapshot = 0x31,
    ScrollbackReq = 0x32,
    ScrollbackData = 0x33,
    SessionExited = 0x34,
    Layout = 0x35,
    PaneExited = 0x36,
    SessionList2 = 0x37,
    Ping = 0x40,
    Pong = 0x41,
}

/// proto.h `enum proto_err`.
pub const ERR_VERSION: u16 = 1;
pub const ERR_NO_SESSION: u16 = 2;
pub const ERR_NAME_TAKEN: u16 = 3;
pub const ERR_BAD_REQUEST: u16 = 4;
pub const ERR_INTERNAL: u16 = 5;

/// MSG_SELECT_PANE modes (proto.h 0x18 comment).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum SelectMode {
    ById = 0,
    Next = 1,
    Prev = 2,
    Last = 3,
    Up = 4,
    Down = 5,
    Right = 6,
    Left = 7,
    ZoomToggle = 8,
}

/// Wire value for "the active pane" in pane_id fields.
pub const PANE_ACTIVE: u8 = 255;

/// A malformed daemon→client payload. Carries a static reason for logs;
/// the caller's only real move is to drop the frame (or, for framing-level
/// violations, the connection — but that is `DecodeError`, not this).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ParseError(pub &'static str);

// ---- client → daemon encoders (complete frames, ready to write) ----

/// MSG_HELLO: u16 ver, u16 flags (proto.h; parsed at server.c handle
/// with caps read only when len >= 4).
pub fn hello(flags: u16) -> Vec<u8> {
    let mut p = Vec::with_capacity(4);
    p.extend_from_slice(&crate::PROTO_VERSION.to_le_bytes());
    p.extend_from_slice(&flags.to_le_bytes());
    encode_frame(MsgType::Hello as u8, &p)
}

/// MSG_LIST_SESSIONS2: empty (proto.h 0x1a).
pub fn list_sessions2() -> Vec<u8> {
    encode_frame(MsgType::ListSessions2 as u8, &[])
}

/// MSG_LIST_SESSIONS: empty (proto.h 0x10) — v1 fallback when the daemon
/// skips the LIST2 request (old daemon, unknown type).
pub fn list_sessions() -> Vec<u8> {
    encode_frame(MsgType::ListSessions as u8, &[])
}

/// MSG_NEW_SESSION: u16 cols, u16 rows, u8 nlen, name, u16 argv_bytes,
/// argv where argv is NUL-*terminated* strings back to back (attach.c
/// send_new copies strlen+1 per arg; server.c walks q += strlen(q)+1).
/// Empty argv → daemon runs $SHELL.
pub fn new_session(cols: u16, rows: u16, name: &str, argv: &[&str]) -> Result<Vec<u8>, ParseError> {
    let nlen = name.len();
    if nlen == 0 || nlen > 63 {
        return Err(ParseError("session name must be 1..=63 bytes"));
    }
    let mut p = Vec::new();
    p.extend_from_slice(&cols.to_le_bytes());
    p.extend_from_slice(&rows.to_le_bytes());
    p.push(nlen as u8);
    p.extend_from_slice(name.as_bytes());
    let mut argv_bytes: Vec<u8> = Vec::new();
    for a in argv {
        if a.as_bytes().contains(&0) {
            return Err(ParseError("argv strings cannot contain NUL"));
        }
        argv_bytes.extend_from_slice(a.as_bytes());
        argv_bytes.push(0);
    }
    if argv_bytes.len() > u16::MAX as usize {
        return Err(ParseError("argv too long for u16 length field"));
    }
    p.extend_from_slice(&(argv_bytes.len() as u16).to_le_bytes());
    p.extend_from_slice(&argv_bytes);
    Ok(encode_frame(MsgType::NewSession as u8, &p))
}

/// MSG_KILL_SESSION: u8 nlen, name (proto.h 0x13).
pub fn kill_session(name: &str) -> Result<Vec<u8>, ParseError> {
    if name.is_empty() || name.len() > 63 {
        return Err(ParseError("session name must be 1..=63 bytes"));
    }
    let mut p = Vec::with_capacity(1 + name.len());
    p.push(name.len() as u8);
    p.extend_from_slice(name.as_bytes());
    Ok(encode_frame(MsgType::KillSession as u8, &p))
}

/// MSG_ATTACH: u16 cols, u16 rows, u8 pane_id, u8 nlen, name.
/// pane_id 0 = "don't change" (the pre-pane client always wrote 0, so 0
/// can never mean "select pane 0"); 255 = active (proto.h 0x14 comment).
pub fn attach(cols: u16, rows: u16, pane_id: u8, name: &str) -> Result<Vec<u8>, ParseError> {
    if name.is_empty() || name.len() > 63 {
        return Err(ParseError("session name must be 1..=63 bytes"));
    }
    let mut p = Vec::with_capacity(6 + name.len());
    p.extend_from_slice(&cols.to_le_bytes());
    p.extend_from_slice(&rows.to_le_bytes());
    p.push(pane_id);
    p.push(name.len() as u8);
    p.extend_from_slice(name.as_bytes());
    Ok(encode_frame(MsgType::Attach as u8, &p))
}

/// MSG_DETACH: empty (proto.h 0x15).
pub fn detach() -> Vec<u8> {
    encode_frame(MsgType::Detach as u8, &[])
}

/// MSG_RELOAD: empty (proto.h 0x19).
pub fn reload() -> Vec<u8> {
    encode_frame(MsgType::Reload as u8, &[])
}

/// MSG_SPLIT_PANE: u8 stacked, u8 target (255 = active) (proto.h 0x16).
pub fn split_pane(stacked: bool, target: u8) -> Vec<u8> {
    encode_frame(MsgType::SplitPane as u8, &[stacked as u8, target])
}

/// MSG_CLOSE_PANE: u8 pane_id (255 = active) (proto.h 0x17).
pub fn close_pane(pane_id: u8) -> Vec<u8> {
    encode_frame(MsgType::ClosePane as u8, &[pane_id])
}

/// MSG_SELECT_PANE: u8 mode, u8 pane_id (modes 1-8 ignore it)
/// (proto.h 0x18).
pub fn select_pane(mode: SelectMode, pane_id: u8) -> Vec<u8> {
    encode_frame(MsgType::SelectPane as u8, &[mode as u8, pane_id])
}

/// MSG_STDIN_DATA: raw bytes for the PTY (proto.h 0x20).
pub fn stdin_data(bytes: &[u8]) -> Vec<u8> {
    encode_frame(MsgType::StdinData as u8, bytes)
}

/// MSG_RESIZE: u16 cols, u16 rows (proto.h 0x21).
pub fn resize(cols: u16, rows: u16) -> Vec<u8> {
    let mut p = Vec::with_capacity(4);
    p.extend_from_slice(&cols.to_le_bytes());
    p.extend_from_slice(&rows.to_le_bytes());
    encode_frame(MsgType::Resize as u8, &p)
}

/// MSG_SCROLLBACK_REQ: u64 start_seq, u32 max_lines, u8 pane_id — the
/// pane byte is a true append; 255 = active (proto.h 0x32; attach.c
/// send_scrollback_req always sends all 13 bytes).
pub fn scrollback_req(start_seq: u64, max_lines: u32, pane_id: u8) -> Vec<u8> {
    let mut p = Vec::with_capacity(13);
    p.extend_from_slice(&start_seq.to_le_bytes());
    p.extend_from_slice(&max_lines.to_le_bytes());
    p.push(pane_id);
    encode_frame(MsgType::ScrollbackReq as u8, &p)
}

/// MSG_PING: u64 nonce (proto.h 0x40; the daemon echoes the payload back
/// as MSG_PONG verbatim — server.c answers with `p, len`).
pub fn ping(nonce: u64) -> Vec<u8> {
    encode_frame(MsgType::Ping as u8, &nonce.to_le_bytes())
}

// ---- daemon → client parsers (payload in, struct out) ----

fn need(p: &[u8], n: usize, what: &'static str) -> Result<(), ParseError> {
    if p.len() < n {
        Err(ParseError(what))
    } else {
        Ok(())
    }
}

fn u16_at(p: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([p[off], p[off + 1]])
}

fn u32_at(p: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([p[off], p[off + 1], p[off + 2], p[off + 3]])
}

fn u64_at(p: &[u8], off: usize) -> u64 {
    u64::from_le_bytes([
        p[off],
        p[off + 1],
        p[off + 2],
        p[off + 3],
        p[off + 4],
        p[off + 5],
        p[off + 6],
        p[off + 7],
    ])
}

/// MSG_HELLO_OK: u16 ver, then tail-optional additive appends — u32
/// daemon_pid @2, u32 generation @6, u16 server_flags @10 (server.c
/// builds all 12 bytes today; a v1 daemon sent only ver, so every tail
/// field is Option).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HelloOk {
    pub ver: u16,
    pub daemon_pid: Option<u32>,
    pub generation: Option<u32>,
    pub server_flags: Option<u16>,
}

pub fn parse_hello_ok(p: &[u8]) -> Result<HelloOk, ParseError> {
    need(p, 2, "HELLO_OK shorter than its u16 ver")?;
    Ok(HelloOk {
        ver: u16_at(p, 0),
        daemon_pid: (p.len() >= 6).then(|| u32_at(p, 2)),
        generation: (p.len() >= 10).then(|| u32_at(p, 6)),
        server_flags: (p.len() >= 12).then(|| u16_at(p, 10)),
    })
}

/// MSG_ERR: u16 code, u16 msg_len, utf8 msg (proto.h 0x03; server.c
/// client_err). The message is clamped to msg_len even if the payload
/// carries additive trailing bytes someday.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ErrMsg {
    pub code: u16,
    pub msg: String,
}

pub fn parse_err(p: &[u8]) -> Result<ErrMsg, ParseError> {
    need(p, 4, "ERR shorter than code+len")?;
    let code = u16_at(p, 0);
    let mlen = u16_at(p, 2) as usize;
    need(p, 4 + mlen, "ERR msg_len exceeds payload")?;
    Ok(ErrMsg {
        code,
        msg: String::from_utf8_lossy(&p[4..4 + mlen]).into_owned(),
    })
}

/// MSG_SNAPSHOT: u16 cols, u16 rows, u64 sb_lines, then a length-implicit
/// ANSI blob from offset 12 (proto.h 0x31: FROZEN — can never grow a
/// field, appended bytes would land on the user's screen).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Snapshot<'a> {
    pub cols: u16,
    pub rows: u16,
    pub sb_lines: u64,
    pub blob: &'a [u8],
}

pub fn parse_snapshot(p: &[u8]) -> Result<Snapshot<'_>, ParseError> {
    need(p, 12, "SNAPSHOT shorter than its fixed 12-byte head")?;
    Ok(Snapshot {
        cols: u16_at(p, 0),
        rows: u16_at(p, 2),
        sb_lines: u64_at(p, 4),
        blob: &p[12..],
    })
}

/// One SESSION_LIST2 entry (proto.h 0x37 comment; server.c handle_list2):
/// u8 nlen, name, u16 view_cols, u16 view_rows, u8 alive, u8 nclients,
/// u32 pid, u32 exit_status, u8 npanes, u8 zoomed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionEntry {
    pub name: String,
    pub view_cols: u16,
    pub view_rows: u16,
    pub alive: bool,
    pub nclients: u8,
    pub pid: u32,
    pub exit_status: u32,
    /// None when the entry predates the field (never today; defensive
    /// symmetry with the skip rule).
    pub npanes: Option<u8>,
    pub zoomed: Option<bool>,
}

/// MSG_SESSION_LIST2: u16 count, then {u16 entry_len, entry}...
/// Entries are advanced by entry_len — NEVER by the sum of parsed fields
/// — so future daemons can append per-entry fields (that skip rule is
/// the whole reason LIST2 exists; proto.h 0x37 comment).
pub fn parse_session_list2(p: &[u8]) -> Result<Vec<SessionEntry>, ParseError> {
    need(p, 2, "SESSION_LIST2 shorter than its count")?;
    let count = u16_at(p, 0) as usize;
    let mut out = Vec::with_capacity(count);
    let mut off = 2usize;
    for _ in 0..count {
        need(p, off + 2, "SESSION_LIST2 truncated at an entry_len")?;
        let entry_len = u16_at(p, off) as usize;
        off += 2;
        need(
            p,
            off + entry_len,
            "SESSION_LIST2 entry_len exceeds payload",
        )?;
        let e = &p[off..off + entry_len];
        out.push(parse_list2_entry(e)?);
        off += entry_len; // by length prefix, not by parsed size
    }
    Ok(out)
}

fn parse_list2_entry(e: &[u8]) -> Result<SessionEntry, ParseError> {
    need(e, 1, "LIST2 entry shorter than nlen")?;
    let nlen = e[0] as usize;
    // 15 = nlen byte + u16 cols + u16 rows + alive + nclients + u32 pid
    // + u32 exit_status (written as a single constant: the spelled-out
    // sum contains 2 + 2, which a mutation to 2 * 2 leaves unchanged —
    // an equivalent mutant this form removes).
    let fixed = 15 + nlen;
    need(e, fixed, "LIST2 entry shorter than its v1 fields")?;
    let name = String::from_utf8_lossy(&e[1..1 + nlen]).into_owned();
    let b = 1 + nlen;
    Ok(SessionEntry {
        name,
        view_cols: u16_at(e, b),
        view_rows: u16_at(e, b + 2),
        alive: e[b + 4] != 0,
        nclients: e[b + 5],
        pid: u32_at(e, b + 6),
        exit_status: u32_at(e, b + 10),
        npanes: (e.len() > fixed).then(|| e[fixed]),
        zoomed: (e.len() >= fixed + 2).then(|| e[fixed + 1] != 0),
    })
}

/// MSG_SESSION_LIST (v1, 0x11): u16 count then positional entries with NO
/// per-entry length (server.c handle_list): u8 nlen, name, u16 view_cols,
/// u16 view_rows, u8 alive, u8 nclients, u32 pid, u32 exit_status.
/// Fallback for old daemons that skip the LIST2 request.
pub fn parse_session_list(p: &[u8]) -> Result<Vec<SessionEntry>, ParseError> {
    need(p, 2, "SESSION_LIST shorter than its count")?;
    let count = u16_at(p, 0) as usize;
    let mut out = Vec::with_capacity(count);
    let mut off = 2usize;
    for _ in 0..count {
        need(p, off + 1, "SESSION_LIST truncated at an nlen")?;
        let nlen = p[off] as usize;
        need(p, off + 1 + nlen + 14, "SESSION_LIST entry truncated")?;
        let name = String::from_utf8_lossy(&p[off + 1..off + 1 + nlen]).into_owned();
        let b = off + 1 + nlen;
        out.push(SessionEntry {
            name,
            view_cols: u16_at(p, b),
            view_rows: u16_at(p, b + 2),
            alive: p[b + 4] != 0,
            nclients: p[b + 5],
            pid: u32_at(p, b + 6),
            exit_status: u32_at(p, b + 10),
            npanes: None,
            zoomed: None,
        });
        off = b + 14;
    }
    Ok(out)
}

/// One pane rect in MSG_LAYOUT (cell coordinates).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PaneRect {
    pub id: u8,
    pub x: u16,
    pub y: u16,
    pub cols: u16,
    pub rows: u16,
}

/// MSG_LAYOUT: u16 view_cols, u16 view_rows, u8 active_id, u8 npanes,
/// then per pane u8 id, u16 x, u16 y, u16 cols, u16 rows (proto.h 0x35;
/// session.c session_send_layout). Sent only to CLIENT_CAP_PANES clients.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Layout {
    pub view_cols: u16,
    pub view_rows: u16,
    pub active_id: u8,
    pub panes: Vec<PaneRect>,
}

pub fn parse_layout(p: &[u8]) -> Result<Layout, ParseError> {
    need(p, 6, "LAYOUT shorter than its fixed head")?;
    let npanes = p[5] as usize;
    need(
        p,
        6 + npanes * 9,
        "LAYOUT truncated inside its pane records",
    )?;
    let mut panes = Vec::with_capacity(npanes);
    for i in 0..npanes {
        let b = 6 + i * 9;
        panes.push(PaneRect {
            id: p[b],
            x: u16_at(p, b + 1),
            y: u16_at(p, b + 3),
            cols: u16_at(p, b + 5),
            rows: u16_at(p, b + 7),
        });
    }
    Ok(Layout {
        view_cols: u16_at(p, 0),
        view_rows: u16_at(p, 2),
        active_id: p[4],
        panes,
    })
}

/// MSG_SCROLLBACK_DATA: u64 first_seq, u32 nlines, then {u32 len, bytes}
/// per line (proto.h 0x33; server.c emits `emitted` as nlines and stops
/// adding lines that would overflow — nlines is authoritative).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ScrollbackData {
    pub first_seq: u64,
    pub lines: Vec<Vec<u8>>,
}

pub fn parse_scrollback_data(p: &[u8]) -> Result<ScrollbackData, ParseError> {
    need(p, 12, "SCROLLBACK_DATA shorter than its fixed head")?;
    let nlines = u32_at(p, 8) as usize;
    let mut lines = Vec::with_capacity(nlines.min(1000));
    let mut off = 12usize;
    for _ in 0..nlines {
        need(p, off + 4, "SCROLLBACK_DATA truncated at a line length")?;
        let len = u32_at(p, off) as usize;
        off += 4;
        need(p, off + len, "SCROLLBACK_DATA line length exceeds payload")?;
        lines.push(p[off..off + len].to_vec());
        off += len;
    }
    Ok(ScrollbackData {
        first_seq: u64_at(p, 0),
        lines,
    })
}

/// MSG_SESSION_EXITED: i32 exit_status (proto.h 0x34).
pub fn parse_session_exited(p: &[u8]) -> Result<i32, ParseError> {
    need(p, 4, "SESSION_EXITED shorter than its i32")?;
    Ok(u32_at(p, 0) as i32)
}

/// MSG_PANE_EXITED: u8 pane_id, i32 exit_status (proto.h 0x36).
pub fn parse_pane_exited(p: &[u8]) -> Result<(u8, i32), ParseError> {
    need(p, 5, "PANE_EXITED shorter than id+status")?;
    Ok((p[0], u32_at(p, 1) as i32))
}

/// MSG_PONG: u64 nonce (the daemon echoes the PING payload verbatim).
pub fn parse_pong(p: &[u8]) -> Result<u64, ParseError> {
    need(p, 8, "PONG shorter than its nonce")?;
    Ok(u64_at(p, 0))
}
