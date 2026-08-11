//! Chain-verification fixtures (design: app/design/hook-log.md).
//! Synthetic events throughout.

use hooks_model::{line_hash, verify_chain};

/// Build a VALID chain of n lines the way the wrapper snippet does.
fn valid_chain(n: usize) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut prev = "GENESIS".to_string();
    for i in 0..n {
        let line = format!(
            r#"{{"ts":"2026-08-11T12:00:0{i}Z","hook":"guard.sh","event":"PreToolUse","tool":"Bash","decision":"allow","reason":"r{i}","prev":"{prev}"}}"#
        );
        prev = line_hash(&line);
        out.push(line);
    }
    out
}

fn refs(v: &[String]) -> Vec<&str> {
    v.iter().map(|s| s.as_str()).collect()
}

#[test]
fn a_valid_chain_verifies_and_yields_every_event() {
    let lines = valid_chain(3);
    let r = verify_chain(&refs(&lines));
    assert_eq!(r.break_at, None);
    assert_eq!(r.malformed, 0);
    assert_eq!(r.events.len(), 3);
    assert_eq!(r.events[0].decision, "allow");
    assert_eq!(r.events[2].reason, "r2");
}

#[test]
fn an_empty_log_is_intact() {
    let r = verify_chain(&[]);
    assert_eq!(r.break_at, None);
    assert_eq!(r.events.len(), 0);
}

#[test]
fn a_wrong_genesis_breaks_at_line_zero() {
    let mut lines = valid_chain(2);
    lines[0] = lines[0].replace("GENESIS", "genesis");
    // Line 1's prev now also mismatches (it hashes the ORIGINAL line 0),
    // but the report pins the FIRST break.
    let r = verify_chain(&refs(&lines));
    assert_eq!(r.break_at, Some(0));
}

#[test]
fn an_edited_middle_line_breaks_exactly_there_and_later_events_still_display() {
    let mut lines = valid_chain(4);
    // The classic tamper: change a reason after the fact without
    // recomputing anything. Line 1's own prev is still right; line 2's
    // prev no longer matches the EDITED bytes of line 1.
    lines[1] = lines[1].replace(r#""reason":"r1""#, r#""reason":"edited""#);
    let r = verify_chain(&refs(&lines));
    assert_eq!(
        r.break_at,
        Some(2),
        "the break lands on the first line whose prev fails"
    );
    assert_eq!(r.events.len(), 4, "history after the break still displays");
    assert_eq!(r.malformed, 0);
}

#[test]
fn a_non_json_line_is_malformed_breaks_the_chain_and_is_bridged_by_raw_hash() {
    let mut lines = valid_chain(1);
    let garbage = "corrupted partial wri".to_string();
    // A writer that resumed correctly would hash the garbage's RAW
    // bytes — the chain runs over bytes, not over parsed JSON.
    let next = format!(
        r#"{{"ts":"2026-08-11T12:00:09Z","hook":"g.sh","event":"Stop","tool":"","decision":"allow","reason":"","prev":"{}"}}"#,
        line_hash(&garbage)
    );
    lines.push(garbage);
    lines.push(next);
    let r = verify_chain(&refs(&lines));
    assert_eq!(r.malformed, 1);
    assert_eq!(r.break_at, Some(1), "the garbage line itself is the break");
    assert_eq!(
        r.events.len(),
        2,
        "the garbage line yields no event; its neighbors do"
    );
}

#[test]
fn interleaved_concurrent_appends_are_detected() {
    // Two hooks that both read the same "last line" and appended without
    // the flock: the second one's prev points one line too far back.
    let lines = valid_chain(2);
    let stale_prev = line_hash(&lines[0]);
    let interleaved = format!(
        r#"{{"ts":"2026-08-11T12:00:07Z","hook":"other.sh","event":"PreToolUse","tool":"Bash","decision":"block","reason":"x","prev":"{stale_prev}"}}"#
    );
    let mut all = lines;
    all.push(interleaved);
    let r = verify_chain(&refs(&all));
    assert_eq!(r.break_at, Some(2));
}
