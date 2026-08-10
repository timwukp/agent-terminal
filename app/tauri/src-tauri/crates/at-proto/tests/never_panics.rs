//! Property tests: no parser may panic on arbitrary bytes — the Rust
//! analog of the C repo's libFuzzer targets. Parsers return Err on junk;
//! they never index out of bounds, overflow, or OOM on lying lengths.

use at_proto::*;
use proptest::prelude::*;

proptest! {
    #![proptest_config(ProptestConfig::with_cases(2048))]

    #[test]
    fn decoder_never_panics(chunks in prop::collection::vec(
        prop::collection::vec(any::<u8>(), 0..64), 0..32)) {
        let mut d = Decoder::new();
        for c in &chunks {
            d.feed(c);
            // Drain until quiescent; errors are fine, panics are not.
            while let Ok(Some(_)) = d.next_frame() {}
        }
    }

    #[test]
    fn header_decode_never_panics(bytes in prop::collection::vec(any::<u8>(), 0..16)) {
        let _ = decode_header(&bytes);
    }

    #[test]
    fn parsers_never_panic(bytes in prop::collection::vec(any::<u8>(), 0..512)) {
        let _ = parse_hello_ok(&bytes);
        let _ = parse_err(&bytes);
        let _ = parse_snapshot(&bytes);
        let _ = parse_session_list2(&bytes);
        let _ = parse_session_list(&bytes);
        let _ = parse_layout(&bytes);
        let _ = parse_scrollback_data(&bytes);
        let _ = parse_session_exited(&bytes);
        let _ = parse_pane_exited(&bytes);
        let _ = parse_pong(&bytes);
    }

    /// Lying interior lengths must error, never allocate absurdly: a
    /// SCROLLBACK_DATA claiming u32::MAX lines in a tiny payload.
    #[test]
    fn lying_counts_bounded(nlines in 0u32..=u32::MAX) {
        let mut p = Vec::new();
        p.extend_from_slice(&0u64.to_le_bytes());
        p.extend_from_slice(&nlines.to_le_bytes());
        let r = parse_scrollback_data(&p);
        if nlines == 0 {
            prop_assert!(r.is_ok());
        } else {
            prop_assert!(r.is_err());
        }
    }

    /// Round-trip: any payload that encodes must stream-decode back
    /// identically through arbitrary chunking.
    #[test]
    fn encode_stream_roundtrip(
        msg_type in any::<u8>(),
        payload in prop::collection::vec(any::<u8>(), 0..2048),
        cut in any::<prop::sample::Index>(),
    ) {
        let frame = encode_frame(msg_type, &payload);
        let k = cut.index(frame.len() + 1);
        let mut d = Decoder::new();
        d.feed(&frame[..k]);
        let early = d.next_frame().unwrap();
        d.feed(&frame[k..]);
        let f = match early {
            Some(f) => f,
            None => d.next_frame().unwrap().expect("whole frame fed"),
        };
        prop_assert_eq!(f.msg_type, msg_type);
        prop_assert_eq!(f.payload, payload);
    }
}
