//! Streaming decoder tests: split-across-reads reassembly, multi-frame
//! feeds, violation poisoning — the Rust analog of proto_read_frame's
//! contract and the C scanner's split-read discipline.

use at_proto::*;

#[test]
fn whole_frame_roundtrip() {
    let mut d = Decoder::new();
    d.feed(&encode_frame(0x30, b"bytes"));
    let f = d.next_frame().unwrap().unwrap();
    assert_eq!(f.msg_type, 0x30);
    assert_eq!(f.payload, b"bytes");
    assert_eq!(d.next_frame().unwrap(), None);
    assert_eq!(d.buffered(), 0);
}

#[test]
fn buffered_reports_pending_bytes() {
    // buffered() is backpressure accounting: it must track what is
    // actually queued, not return a constant.
    let mut d = Decoder::new();
    d.feed(&[1, 2, 3]);
    assert_eq!(d.buffered(), 3);
    d.feed(&[4]);
    assert_eq!(d.buffered(), 4);
}

#[test]
fn one_byte_at_a_time() {
    // A frame must survive read() boundaries: one byte per feed.
    let frame = encode_frame(0x31, b"snapshot-blob");
    let mut d = Decoder::new();
    for (i, b) in frame.iter().enumerate() {
        assert_eq!(d.next_frame().unwrap(), None, "no frame before byte {i}");
        d.feed(&[*b]);
    }
    let f = d.next_frame().unwrap().unwrap();
    assert_eq!(f.payload, b"snapshot-blob");
}

#[test]
fn split_inside_header_and_inside_payload() {
    let frame = encode_frame(0x20, b"stdin");
    let mut d = Decoder::new();
    d.feed(&frame[..3]); // mid-header
    assert_eq!(d.next_frame().unwrap(), None);
    d.feed(&frame[3..7]); // header done, mid-payload
    assert_eq!(d.next_frame().unwrap(), None);
    d.feed(&frame[7..]);
    assert_eq!(d.next_frame().unwrap().unwrap().payload, b"stdin");
}

#[test]
fn many_frames_in_one_feed() {
    let mut bytes = Vec::new();
    for i in 0..10u8 {
        bytes.extend_from_slice(&encode_frame(0x30, &[i; 3]));
    }
    let mut d = Decoder::new();
    d.feed(&bytes);
    for i in 0..10u8 {
        assert_eq!(d.next_frame().unwrap().unwrap().payload, vec![i; 3]);
    }
    assert_eq!(d.next_frame().unwrap(), None);
}

#[test]
fn empty_payload_frames() {
    let mut d = Decoder::new();
    d.feed(&encode_frame(0x15, &[])); // DETACH
    d.feed(&encode_frame(0x19, &[])); // RELOAD
    assert_eq!(d.next_frame().unwrap().unwrap().msg_type, 0x15);
    assert_eq!(d.next_frame().unwrap().unwrap().msg_type, 0x19);
}

#[test]
fn max_size_payload_passes() {
    let payload = vec![0xabu8; PROTO_MAX_PAYLOAD];
    let mut d = Decoder::new();
    d.feed(&encode_frame(0x30, &payload));
    assert_eq!(
        d.next_frame().unwrap().unwrap().payload.len(),
        PROTO_MAX_PAYLOAD
    );
}

#[test]
fn oversize_poisons_permanently() {
    let mut bad = ((PROTO_MAX_PAYLOAD as u32) + 1).to_le_bytes().to_vec();
    bad.push(0x30);
    let mut d = Decoder::new();
    d.feed(&bad);
    assert!(d.next_frame().is_err());
    // Poisoned: a valid frame fed afterwards must NOT resurrect the
    // stream — resync inside length-framed bytes is impossible, which is
    // why the C side disconnects.
    d.feed(&encode_frame(0x40, &7u64.to_le_bytes()));
    assert!(d.next_frame().is_err());
    assert!(d.next_frame().is_err());
}

#[test]
fn violation_detected_from_header_alone() {
    // The oversize is visible in the 5 header bytes — the decoder must
    // not wait for (unbounded) payload before erroring.
    let mut hdr = u32::MAX.to_le_bytes().to_vec();
    hdr.push(0x01);
    let mut d = Decoder::new();
    d.feed(&hdr);
    assert!(d.next_frame().is_err());
}

/// Interleaving pattern from the wire: OUTPUT, LAYOUT, OUTPUT — as a
/// composited pane transition produces — split at awkward offsets.
#[test]
fn realistic_interleave_split_awkwardly() {
    let frames = [
        encode_frame(0x30, b"\x1b[2J\x1b[H frame one"),
        encode_frame(0x35, &{
            let mut p = Vec::new();
            p.extend_from_slice(&160u16.to_le_bytes());
            p.extend_from_slice(&40u16.to_le_bytes());
            p.push(0);
            p.push(1);
            p.push(0);
            p.extend_from_slice(&0u16.to_le_bytes());
            p.extend_from_slice(&0u16.to_le_bytes());
            p.extend_from_slice(&160u16.to_le_bytes());
            p.extend_from_slice(&40u16.to_le_bytes());
            p
        }),
        encode_frame(0x30, b"more bytes"),
    ];
    let all: Vec<u8> = frames.iter().flatten().copied().collect();
    // Split at every possible single cut point; both halves must always
    // reassemble to the same three frames.
    for cut in 0..=all.len() {
        let mut d = Decoder::new();
        d.feed(&all[..cut]);
        let mut got = Vec::new();
        while let Some(f) = d.next_frame().unwrap() {
            got.push(f);
        }
        d.feed(&all[cut..]);
        while let Some(f) = d.next_frame().unwrap() {
            got.push(f);
        }
        assert_eq!(got.len(), 3, "cut at {cut}");
        assert_eq!(got[0].msg_type, 0x30);
        assert_eq!(got[1].msg_type, 0x35);
        assert_eq!(got[2].payload, b"more bytes");
    }
}
