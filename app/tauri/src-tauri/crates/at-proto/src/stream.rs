//! Incremental frame decoder over a byte stream: feed arbitrary read()
//! chunks, get whole frames out. Mirrors proto_read_frame's contract
//! (proto.c): 1 MiB cap is a violation → the decoder poisons itself and
//! the caller must disconnect; short data just waits for more.

use crate::{DecodeError, PROTO_HDR_SIZE, PROTO_MAX_PAYLOAD};

/// One whole frame as read off the wire.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub msg_type: u8,
    pub payload: Vec<u8>,
}

/// Streaming decoder. Bytes go in via [`Decoder::feed`]; complete frames
/// come out of [`Decoder::next_frame`]. After a protocol violation every
/// further call returns the same error (a violating connection is dead;
/// resynchronizing inside a byte stream with length-framed records is
/// not possible, which is exactly why the C side disconnects).
#[derive(Debug, Default)]
pub struct Decoder {
    buf: Vec<u8>,
    poisoned: Option<DecodeError>,
}

impl Decoder {
    pub fn new() -> Self {
        Self::default()
    }

    /// Append bytes read from the socket.
    pub fn feed(&mut self, bytes: &[u8]) {
        if self.poisoned.is_none() {
            self.buf.extend_from_slice(bytes);
        }
    }

    /// Pop the next complete frame, if one is buffered.
    /// `Ok(None)` = need more bytes. `Err` = protocol violation:
    /// disconnect (and every subsequent call repeats the error).
    pub fn next_frame(&mut self) -> Result<Option<Frame>, DecodeError> {
        if let Some(e) = self.poisoned {
            return Err(e);
        }
        if self.buf.len() < PROTO_HDR_SIZE {
            return Ok(None);
        }
        let payload_len =
            u32::from_le_bytes([self.buf[0], self.buf[1], self.buf[2], self.buf[3]]) as usize;
        if payload_len > PROTO_MAX_PAYLOAD {
            let e = DecodeError::Oversized {
                payload_len: payload_len as u32,
            };
            self.poisoned = Some(e);
            return Err(e);
        }
        if self.buf.len() < PROTO_HDR_SIZE + payload_len {
            return Ok(None);
        }
        let msg_type = self.buf[4];
        let payload = self.buf[PROTO_HDR_SIZE..PROTO_HDR_SIZE + payload_len].to_vec();
        self.buf.drain(..PROTO_HDR_SIZE + payload_len);
        Ok(Some(Frame { msg_type, payload }))
    }

    /// Bytes currently buffered (tests and backpressure accounting).
    pub fn buffered(&self) -> usize {
        self.buf.len()
    }
}
