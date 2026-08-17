//! Pure (sans-io) codec for the agent-terminal wire protocol.
//!
//! `src/common/proto.h` at the repo root is the single source of truth;
//! every constant and layout here cites its anchor there. Frame layout
//! (all integers little-endian):
//!
//! ```text
//! offset  size  field
//! 0       4     payload_len  (u32, excludes this 5-byte header)
//! 4       1     type
//! 5       N     payload      (N == payload_len, max PROTO_MAX_PAYLOAD)
//! ```
//!
//! Semantics mirrored from proto.c / the daemon:
//! - an oversized `payload_len` is a protocol violation: disconnect
//!   (`proto_read_frame` returns -1);
//! - unknown frame *types* are skipped by the receiver;
//! - unknown *trailing payload bytes* are ignored (additive evolution);
//! - tail-optional fields (HELLO_OK) parse only when present.

mod msg;
mod stream;

pub use msg::*;
pub use stream::{Decoder, Frame};

/// proto.h: `PROTO_VERSION`
pub const PROTO_VERSION: u16 = 1;
/// proto.h: `PROTO_HDR_SIZE`
pub const PROTO_HDR_SIZE: usize = 5;
/// proto.h: `PROTO_MAX_PAYLOAD` — a peer violating this is disconnected.
pub const PROTO_MAX_PAYLOAD: usize = 1 << 20;

/// proto.h: `CLIENT_CAP_PANES` (MSG_HELLO u16 flags).
pub const CLIENT_CAP_PANES: u16 = 0x0001;
/// proto.h: `CLIENT_CAP_SESSION_EVENTS` — ask for MSG_SESSIONS_CHANGED.
/// Its own bit rather than a rider on `CLIENT_CAP_PANES`, because a client
/// that set only 0x0001 was built before 0x39 existed.
pub const CLIENT_CAP_SESSION_EVENTS: u16 = 0x0002;
/// proto.h: `SERVER_CAP_PANES` (MSG_HELLO_OK u16 server_flags @10).
pub const SERVER_CAP_PANES: u16 = 0x0001;
/// proto.h: `SERVER_CAP_SESSION_EVENTS` — this daemon will send
/// MSG_SESSIONS_CHANGED. Worth checking rather than assuming: without it,
/// "the daemon will notify me" and "the daemon is too old and I must keep
/// polling" are the same observation, which is silence.
pub const SERVER_CAP_SESSION_EVENTS: u16 = 0x0002;

/// A decoded frame header.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FrameHeader {
    pub payload_len: u32,
    pub msg_type: u8,
}

/// Errors the frame decoder can report. `Oversized` is a protocol
/// violation: the caller must disconnect (mirrors proto_read_frame
/// returning -1).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DecodeError {
    /// payload_len exceeds PROTO_MAX_PAYLOAD.
    Oversized { payload_len: u32 },
}

/// Try to decode a frame header from the front of `buf`.
/// Returns `Ok(None)` when fewer than PROTO_HDR_SIZE bytes are available
/// (need more), `Ok(Some(h))` on success, `Err` on a protocol violation.
pub fn decode_header(buf: &[u8]) -> Result<Option<FrameHeader>, DecodeError> {
    if buf.len() < PROTO_HDR_SIZE {
        return Ok(None);
    }
    let payload_len = u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]);
    if payload_len as usize > PROTO_MAX_PAYLOAD {
        return Err(DecodeError::Oversized { payload_len });
    }
    Ok(Some(FrameHeader {
        payload_len,
        msg_type: buf[4],
    }))
}

/// Encode one complete frame (header + payload). Panics if `payload`
/// exceeds PROTO_MAX_PAYLOAD — callers construct payloads, so an oversize
/// one is a caller bug, not peer input.
pub fn encode_frame(msg_type: u8, payload: &[u8]) -> Vec<u8> {
    assert!(
        payload.len() <= PROTO_MAX_PAYLOAD,
        "oversized payload is a caller bug"
    );
    let mut out = Vec::with_capacity(PROTO_HDR_SIZE + payload.len());
    out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    out.push(msg_type);
    out.extend_from_slice(payload);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The exact capable-client HELLO bytes, pinned as documented in
    /// app/design/protocol-notes.md: 04 00 00 00 01 01 00 01 00.
    #[test]
    fn hello_bytes_pin() {
        let mut payload = Vec::new();
        payload.extend_from_slice(&PROTO_VERSION.to_le_bytes());
        payload.extend_from_slice(&CLIENT_CAP_PANES.to_le_bytes());
        let frame = encode_frame(0x01, &payload);
        assert_eq!(
            frame,
            [0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00]
        );
    }

    /// Both capabilities in one HELLO, pinned as bytes rather than as
    /// `PANES | SESSION_EVENTS`: written that way the assertion would restate
    /// the expression it is checking, and a swap of either constant's value
    /// would leave it passing. 0x0003 in the flags field is what the daemon
    /// reads, and the integration test asserts the mirror of it in HELLO_OK.
    #[test]
    fn hello_both_caps_bytes_pin() {
        let mut payload = Vec::new();
        payload.extend_from_slice(&PROTO_VERSION.to_le_bytes());
        payload.extend_from_slice(&(CLIENT_CAP_PANES | CLIENT_CAP_SESSION_EVENTS).to_le_bytes());
        let frame = encode_frame(0x01, &payload);
        assert_eq!(
            frame,
            [0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x03, 0x00]
        );
    }

    /// The two capability namespaces are separate (client flags vs server
    /// flags) but their bit values are deliberately aligned, so a reader can
    /// compare a request against an advertisement without a translation table.
    #[test]
    fn cap_bits_are_distinct_and_aligned() {
        assert_eq!(CLIENT_CAP_PANES & CLIENT_CAP_SESSION_EVENTS, 0);
        assert_eq!(CLIENT_CAP_PANES, SERVER_CAP_PANES);
        assert_eq!(CLIENT_CAP_SESSION_EVENTS, SERVER_CAP_SESSION_EVENTS);
    }

    #[test]
    fn header_roundtrip() {
        let frame = encode_frame(0x30, b"hello");
        let h = decode_header(&frame).unwrap().unwrap();
        assert_eq!(
            h,
            FrameHeader {
                payload_len: 5,
                msg_type: 0x30
            }
        );
    }

    #[test]
    fn short_buffer_needs_more() {
        assert_eq!(decode_header(&[0, 0, 0, 0]), Ok(None));
    }

    #[test]
    fn oversized_is_violation() {
        let mut buf = ((PROTO_MAX_PAYLOAD as u32) + 1).to_le_bytes().to_vec();
        buf.push(0x30);
        assert_eq!(
            decode_header(&buf),
            Err(DecodeError::Oversized {
                payload_len: PROTO_MAX_PAYLOAD as u32 + 1
            })
        );
    }

    #[test]
    fn max_payload_len_is_legal() {
        let mut buf = (PROTO_MAX_PAYLOAD as u32).to_le_bytes().to_vec();
        buf.push(0x30);
        assert!(decode_header(&buf).unwrap().is_some());
    }
}
