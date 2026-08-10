//! Message-level codec tests: encoder byte pins, parser roundtrips, the
//! tail-optional truncation matrix, and length-prefix skip semantics.
//! Encoder pins are byte-for-byte against the C serializers (attach.c /
//! server.c), not against this crate's own output.

use at_proto::*;

// ---- encoder byte pins ----

#[test]
fn hello_capable_exact_bytes() {
    assert_eq!(
        hello(CLIENT_CAP_PANES),
        [0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00]
    );
}

#[test]
fn hello_flagless_exact_bytes() {
    // What the compat test's python probe sends: ver=1, flags=0.
    assert_eq!(
        hello(0),
        [0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00]
    );
}

#[test]
fn attach_layout_matches_attach_c() {
    // attach.c: u16 cols, u16 rows, u8 pane_id, u8 nlen, name.
    let f = attach(80, 24, 0, "work").unwrap();
    assert_eq!(f[4], 0x14);
    assert_eq!(&f[5..], b"\x50\x00\x18\x00\x00\x04work");
}

#[test]
fn new_session_argv_nul_terminated_each() {
    // server.c walks argv with q += strlen(q)+1, so every arg carries its
    // NUL — including the last (attach.c send_new copies strlen+1).
    let f = new_session(80, 24, "s", &["claude", "-c"]).unwrap();
    assert_eq!(f[4], 0x12);
    let p = &f[5..];
    assert_eq!(&p[0..4], b"\x50\x00\x18\x00");
    assert_eq!(p[4], 1);
    assert_eq!(p[5], b's');
    let argv_bytes = u16::from_le_bytes([p[6], p[7]]) as usize;
    assert_eq!(argv_bytes, 10); // "claude\0-c\0"
    assert_eq!(&p[8..8 + argv_bytes], b"claude\0-c\0");
}

#[test]
fn new_session_empty_argv_means_shell() {
    let f = new_session(80, 24, "s", &[]).unwrap();
    let p = &f[5..];
    assert_eq!(u16::from_le_bytes([p[6], p[7]]), 0);
    assert_eq!(p.len(), 8);
}

#[test]
fn new_session_rejects_embedded_nul_and_bad_names() {
    assert!(new_session(80, 24, "s", &["a\0b"]).is_err());
    assert!(new_session(80, 24, "", &[]).is_err());
    assert!(new_session(80, 24, &"x".repeat(64), &[]).is_err());
}

#[test]
fn name_length_boundary_is_exactly_63() {
    // SESSION_NAME_MAX is 63: 63 accepted, 64 rejected, empty rejected —
    // on every name-carrying encoder.
    let max = "x".repeat(63);
    let over = "x".repeat(64);
    assert!(new_session(80, 24, &max, &[]).is_ok());
    assert!(kill_session(&max).is_ok());
    assert!(attach(80, 24, 0, &max).is_ok());
    assert!(new_session(80, 24, &over, &[]).is_err());
    assert!(kill_session(&over).is_err());
    assert!(attach(80, 24, 0, &over).is_err());
    assert!(kill_session("").is_err());
    assert!(attach(80, 24, 0, "").is_err());
}

#[test]
fn argv_length_boundary_is_exactly_u16_max() {
    // argv_bytes rides a u16: 65535 encodable, 65536 not. One arg of
    // N chars contributes N+1 bytes (its NUL).
    let fits = "a".repeat(u16::MAX as usize - 1);
    let f = new_session(80, 24, "s", &[&fits]).unwrap();
    let p = &f[5..];
    assert_eq!(u16::from_le_bytes([p[6], p[7]]), u16::MAX);
    let over = "a".repeat(u16::MAX as usize);
    assert!(new_session(80, 24, "s", &[&over]).is_err());
}

#[test]
fn scrollback_req_sends_all_13_bytes() {
    // attach.c send_scrollback_req: fixed 13 bytes, pane byte always sent.
    let f = scrollback_req(7, 100, PANE_ACTIVE);
    assert_eq!(f[4], 0x32);
    assert_eq!(f.len(), 5 + 13);
    assert_eq!(f[5 + 12], 255);
}

#[test]
fn select_pane_modes_match_proto_h() {
    for (mode, want) in [
        (SelectMode::ById, 0u8),
        (SelectMode::Next, 1),
        (SelectMode::Prev, 2),
        (SelectMode::Last, 3),
        (SelectMode::Up, 4),
        (SelectMode::Down, 5),
        (SelectMode::Right, 6),
        (SelectMode::Left, 7),
        (SelectMode::ZoomToggle, 8),
    ] {
        let f = select_pane(mode, 3);
        assert_eq!(&f[5..], &[want, 3]);
    }
}

#[test]
fn simple_encoders_exact() {
    assert_eq!(&list_sessions2()[4..], &[0x1a]);
    assert_eq!(&list_sessions()[4..], &[0x10]);
    assert_eq!(&detach()[4..], &[0x15]);
    assert_eq!(&reload()[4..], &[0x19]);
    assert_eq!(&split_pane(true, 255)[5..], &[1, 255]);
    assert_eq!(&split_pane(false, 2)[5..], &[0, 2]);
    assert_eq!(&close_pane(255)[5..], &[255]);
    assert_eq!(&resize(120, 40)[5..], &[120, 0, 40, 0]);
    assert_eq!(&stdin_data(b"hi")[5..], b"hi");
    assert_eq!(&kill_session("w").unwrap()[5..], b"\x01w");
    assert_eq!(&ping(0x0102030405060708)[5..], &[8, 7, 6, 5, 4, 3, 2, 1]);
}

// ---- HELLO_OK truncation matrix ----

fn hello_ok_bytes() -> Vec<u8> {
    // server.c: u16 ver=1, u32 pid, u32 generation, u16 server_flags.
    let mut p = Vec::new();
    p.extend_from_slice(&1u16.to_le_bytes());
    p.extend_from_slice(&4242u32.to_le_bytes());
    p.extend_from_slice(&3u32.to_le_bytes());
    p.extend_from_slice(&SERVER_CAP_PANES.to_le_bytes());
    p
}

#[test]
fn hello_ok_full_12_bytes() {
    let h = parse_hello_ok(&hello_ok_bytes()).unwrap();
    assert_eq!(h.ver, 1);
    assert_eq!(h.daemon_pid, Some(4242));
    assert_eq!(h.generation, Some(3));
    assert_eq!(h.server_flags, Some(SERVER_CAP_PANES));
}

#[test]
fn hello_ok_tail_truncation_matrix() {
    // Every prefix a historical daemon could send. Boundaries per
    // server.c offsets: ver@0, pid@2, generation@6, server_flags@10.
    let full = hello_ok_bytes();
    for len in 0..=full.len() {
        let r = parse_hello_ok(&full[..len]);
        if len < 2 {
            assert!(r.is_err(), "len {len}: ver missing must be an error");
            continue;
        }
        let h = r.unwrap();
        assert_eq!(h.daemon_pid.is_some(), len >= 6, "pid at len {len}");
        assert_eq!(h.generation.is_some(), len >= 10, "generation at len {len}");
        assert_eq!(h.server_flags.is_some(), len >= 12, "flags at len {len}");
    }
}

// ---- ERR ----

#[test]
fn err_parses_and_clamps_to_msg_len() {
    let mut p = vec![2, 0, 5, 0];
    p.extend_from_slice(b"no such session"); // msg_len says 5, extra is additive tail
    let e = parse_err(&p).unwrap();
    assert_eq!(e.code, ERR_NO_SESSION);
    assert_eq!(e.msg, "no su");
}

#[test]
fn err_truncated_msg_is_error() {
    assert!(parse_err(&[2, 0, 10, 0, b'x']).is_err());
    assert!(parse_err(&[2, 0]).is_err());
}

// ---- SNAPSHOT ----

#[test]
fn snapshot_blob_is_length_implicit() {
    let mut p = Vec::new();
    p.extend_from_slice(&80u16.to_le_bytes());
    p.extend_from_slice(&24u16.to_le_bytes());
    p.extend_from_slice(&177u64.to_le_bytes());
    p.extend_from_slice(b"\x1b[2J\x1b[Hscreen");
    let s = parse_snapshot(&p).unwrap();
    assert_eq!((s.cols, s.rows, s.sb_lines), (80, 24, 177));
    assert_eq!(s.blob, b"\x1b[2J\x1b[Hscreen");
    // Empty blob is legal (blank session).
    assert_eq!(parse_snapshot(&p[..12]).unwrap().blob, b"");
    assert!(parse_snapshot(&p[..11]).is_err());
}

// ---- SESSION_LIST2 ----

fn list2_entry(name: &str, npanes: u8, zoomed: u8, extra_tail: &[u8]) -> Vec<u8> {
    // server.c handle_list2 layout, plus an optional future-field tail.
    let mut e = vec![name.len() as u8];
    e.extend_from_slice(name.as_bytes());
    e.extend_from_slice(&160u16.to_le_bytes());
    e.extend_from_slice(&40u16.to_le_bytes());
    e.push(1); // alive
    e.push(2); // nclients
    e.extend_from_slice(&777u32.to_le_bytes());
    e.extend_from_slice(&0u32.to_le_bytes());
    e.push(npanes);
    e.push(zoomed);
    e.extend_from_slice(extra_tail);
    e
}

fn list2_payload(entries: &[Vec<u8>]) -> Vec<u8> {
    let mut p = (entries.len() as u16).to_le_bytes().to_vec();
    for e in entries {
        p.extend_from_slice(&(e.len() as u16).to_le_bytes());
        p.extend_from_slice(e);
    }
    p
}

#[test]
fn list2_parses_todays_entries() {
    let p = list2_payload(&[
        list2_entry("work", 2, 1, &[]),
        list2_entry("build", 1, 0, &[]),
    ]);
    let v = parse_session_list2(&p).unwrap();
    assert_eq!(v.len(), 2);
    assert_eq!(v[0].name, "work");
    assert_eq!(v[0].view_cols, 160);
    assert_eq!(v[0].view_rows, 40);
    assert!(v[0].alive);
    assert_eq!(v[0].nclients, 2);
    assert_eq!(v[0].npanes, Some(2));
    assert_eq!(v[0].zoomed, Some(true));
    assert_eq!(v[1].name, "build");
    assert_eq!(v[1].pid, 777);
    assert_eq!(v[1].zoomed, Some(false));
}

#[test]
fn list2_dead_entry_alive_flag() {
    // alive is a real boolean off the wire, not a constant: byte 0 must
    // parse as false (exit_status carries the code).
    let mut e = list2_entry("gone", 1, 0, &[]);
    let alive_off = 1 + 4 + 2 + 2; // nlen + name + cols + rows
    e[alive_off] = 0;
    let v = parse_session_list2(&list2_payload(&[e])).unwrap();
    assert!(!v[0].alive);
}

#[test]
fn list2_entry_without_pane_tail_is_v1_shaped() {
    // An entry whose entry_len stops at the v1 fields (no npanes/zoomed
    // bytes): both options None, and no out-of-bounds read. This is the
    // skip rule's mirror image — shorter entries parse too.
    let full = list2_entry("old", 1, 0, &[]);
    let v1 = full[..full.len() - 2].to_vec();
    let v = parse_session_list2(&list2_payload(&[v1])).unwrap();
    assert_eq!(v[0].name, "old");
    assert_eq!(v[0].npanes, None);
    assert_eq!(v[0].zoomed, None);
    // And with only npanes present (one byte of tail):
    let with_np = full[..full.len() - 1].to_vec();
    let v = parse_session_list2(&list2_payload(&[with_np])).unwrap();
    assert_eq!(v[0].npanes, Some(1));
    assert_eq!(v[0].zoomed, None);
}

#[test]
fn list2_skips_unknown_tail_by_entry_len() {
    // A future daemon appends 4 bytes per entry. The parser must land on
    // the NEXT entry via entry_len — summing parsed fields would misread.
    let p = list2_payload(&[
        list2_entry("a", 1, 0, &[9, 9, 9, 9]),
        list2_entry("b", 3, 1, &[9, 9, 9, 9]),
    ]);
    let v = parse_session_list2(&p).unwrap();
    assert_eq!(v.len(), 2);
    assert_eq!(v[1].name, "b");
    assert_eq!(v[1].npanes, Some(3));
}

#[test]
fn list2_truncation_is_error_not_garbage() {
    let p = list2_payload(&[list2_entry("work", 2, 1, &[])]);
    for len in 2..p.len() {
        assert!(
            parse_session_list2(&p[..len]).is_err(),
            "truncation at {len} must error"
        );
    }
    // Count says 1 but no entry follows.
    assert!(parse_session_list2(&[1, 0]).is_err());
    // Empty list is fine.
    assert_eq!(parse_session_list2(&[0, 0]).unwrap().len(), 0);
}

#[test]
fn list2_entry_len_lying_beyond_payload_is_error() {
    let mut p = vec![1, 0];
    p.extend_from_slice(&200u16.to_le_bytes());
    p.extend_from_slice(&[0u8; 20]);
    assert!(parse_session_list2(&p).is_err());
}

// ---- v1 SESSION_LIST fallback ----

fn v1_entry(name: &str, cols: u16, rows: u16, alive: u8, pid: u32) -> Vec<u8> {
    let mut e = vec![name.len() as u8];
    e.extend_from_slice(name.as_bytes());
    e.extend_from_slice(&cols.to_le_bytes());
    e.extend_from_slice(&rows.to_le_bytes());
    e.push(alive);
    e.push(0);
    e.extend_from_slice(&pid.to_le_bytes());
    e.extend_from_slice(&0u32.to_le_bytes());
    e
}

#[test]
fn v1_list_positional_parse() {
    // server.c handle_list: no entry_len, no npanes/zoomed. Two entries
    // of different name lengths pin the positional stride: a wrong
    // per-entry advance parses entry 2 as garbage or errors.
    let mut p = 2u16.to_le_bytes().to_vec();
    p.extend_from_slice(&v1_entry("work", 80, 24, 1, 123));
    p.extend_from_slice(&v1_entry("b", 160, 40, 0, 456));
    let v = parse_session_list(&p).unwrap();
    assert_eq!(v.len(), 2);
    assert_eq!(v[0].name, "work");
    assert_eq!(v[0].view_cols, 80);
    assert_eq!(v[0].view_rows, 24);
    assert!(v[0].alive);
    assert_eq!(v[0].pid, 123);
    assert_eq!(v[0].npanes, None);
    assert_eq!(v[0].zoomed, None);
    assert_eq!(v[1].name, "b");
    assert_eq!(v[1].view_cols, 160);
    assert_eq!(v[1].view_rows, 40);
    assert!(!v[1].alive);
    assert_eq!(v[1].pid, 456);
}

// ---- LAYOUT ----

#[test]
fn layout_parses_pane_rects() {
    // session.c session_send_layout: 6-byte head + 9 bytes per pane.
    let mut p = Vec::new();
    p.extend_from_slice(&160u16.to_le_bytes());
    p.extend_from_slice(&40u16.to_le_bytes());
    p.push(1); // active_id
    p.push(2); // npanes
    for (id, x, y, c, r) in [(0u8, 0u16, 0u16, 79u16, 40u16), (1, 80, 0, 80, 40)] {
        p.push(id);
        p.extend_from_slice(&x.to_le_bytes());
        p.extend_from_slice(&y.to_le_bytes());
        p.extend_from_slice(&c.to_le_bytes());
        p.extend_from_slice(&r.to_le_bytes());
    }
    let l = parse_layout(&p).unwrap();
    assert_eq!(l.active_id, 1);
    assert_eq!(l.panes.len(), 2);
    assert_eq!(
        l.panes[1],
        PaneRect {
            id: 1,
            x: 80,
            y: 0,
            cols: 80,
            rows: 40
        }
    );
    assert!(parse_layout(&p[..p.len() - 1]).is_err());
    assert!(parse_layout(&p[..5]).is_err());
}

// ---- SCROLLBACK_DATA ----

#[test]
fn scrollback_data_lines_roundtrip() {
    let mut p = Vec::new();
    p.extend_from_slice(&40u64.to_le_bytes());
    p.extend_from_slice(&2u32.to_le_bytes());
    for line in [b"first".as_slice(), b"second"] {
        p.extend_from_slice(&(line.len() as u32).to_le_bytes());
        p.extend_from_slice(line);
    }
    let d = parse_scrollback_data(&p).unwrap();
    assert_eq!(d.first_seq, 40);
    assert_eq!(d.lines, vec![b"first".to_vec(), b"second".to_vec()]);
    // nlines says 3 but only 2 present → error, not silence.
    let mut lying = p.clone();
    lying[8..12].copy_from_slice(&3u32.to_le_bytes());
    assert!(parse_scrollback_data(&lying).is_err());
}

// ---- exit / pong ----

#[test]
fn exit_and_pong_parse() {
    assert_eq!(parse_session_exited(&(-9i32).to_le_bytes()).unwrap(), -9);
    let mut p = vec![2u8];
    p.extend_from_slice(&0i32.to_le_bytes());
    assert_eq!(parse_pane_exited(&p).unwrap(), (2, 0));
    assert_eq!(parse_pong(&7u64.to_le_bytes()).unwrap(), 7);
    assert!(parse_session_exited(&[0; 3]).is_err());
    assert!(parse_pane_exited(&[0; 4]).is_err());
    assert!(parse_pong(&[0; 7]).is_err());
}
