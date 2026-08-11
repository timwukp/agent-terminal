//! Table tests over synthetic fixture lines that mirror the observed
//! transcript schema (2026-08, Claude Code 2.x). Synthetic on purpose:
//! real transcripts contain real conversations, and a public repo is no
//! place for those. The shapes — not the content — are what parsing
//! depends on, and each fixture documents the shape it pins.

use claude_watch::{parse_line, Accumulator, Cursor, ParseOutcome, Usage, Watcher, BUCKETS_MAX};

/// One assistant line the way Claude Code writes it: one line per
/// content block, `message.usage` complete, plus sibling fields the
/// parser must ignore (server_tool_use, cache_creation breakdown, …).
fn assistant_line(id: &str, ts: &str, output: u64) -> String {
    format!(
        r#"{{"type":"assistant","timestamp":"{ts}","sessionId":"s-1","cwd":"/opt/proj","message":{{"id":"{id}","model":"claude-fable-5","usage":{{"input_tokens":2,"cache_creation_input_tokens":100,"cache_read_input_tokens":900,"output_tokens":{output},"server_tool_use":{{"web_search_requests":0}},"cache_creation":{{"ephemeral_5m_input_tokens":100}},"service_tier":"standard"}}}}}}"#
    )
}

#[test]
fn assistant_usage_line_parses_every_counter() {
    let ParseOutcome::Event(ev) = parse_line(&assistant_line("m1", "2026-08-11T13:42:57.292Z", 55))
    else {
        panic!("expected an event");
    };
    assert_eq!(ev.message_id, "m1");
    assert_eq!(ev.model, "claude-fable-5");
    assert_eq!(ev.timestamp, "2026-08-11T13:42:57.292Z");
    assert_eq!(
        ev.usage,
        Usage {
            input_tokens: 2,
            output_tokens: 55,
            cache_read_input_tokens: 900,
            cache_creation_input_tokens: 100,
        }
    );
}

#[test]
fn non_assistant_lines_are_irrelevant_not_malformed() {
    // The other line types observed in a real transcript. They are the
    // MAJORITY of lines; counting them as malformed would make the
    // "N unparsed" badge cry wolf on every healthy session.
    for line in [
        r#"{"type":"user","message":{"role":"user"}}"#,
        r#"{"type":"file-history-snapshot","snapshot":{}}"#,
        r#"{"type":"attachment"}"#,
        r#"{"type":"ai-title","title":"t"}"#,
        r#"{"type":"queue-operation"}"#,
        "",
        "   ",
    ] {
        assert!(
            matches!(parse_line(line), ParseOutcome::Irrelevant),
            "line misclassified: {line:?}"
        );
    }
}

#[test]
fn assistant_without_usage_is_irrelevant_but_without_id_is_malformed() {
    // No usage: synthetic/error placeholder — nothing to count.
    let no_usage = r#"{"type":"assistant","message":{"id":"m1","model":"<synthetic>"}}"#;
    assert!(matches!(parse_line(no_usage), ParseOutcome::Irrelevant));
    // Usage but no id: cannot dedup, so it is dropped AND counted as
    // malformed — under-counting with a badge, never a silent 6×.
    let no_id = r#"{"type":"assistant","message":{"usage":{"output_tokens":9}}}"#;
    assert!(matches!(parse_line(no_id), ParseOutcome::Malformed));
}

#[test]
fn garbage_is_malformed() {
    for line in ["not json", "{\"type\":", "{\"type\":\"assistant\"}"] {
        assert!(
            matches!(parse_line(line), ParseOutcome::Malformed),
            "{line:?}"
        );
    }
}

#[test]
fn repeats_of_one_message_count_once() {
    // Claude Code writes one line per content block: the SAME message.id
    // with IDENTICAL usage appeared up to 6× in a measured transcript.
    let mut acc = Accumulator::default();
    for _ in 0..6 {
        acc.feed(&assistant_line("m1", "2026-08-11T13:42:57.292Z", 50));
    }
    assert_eq!(acc.messages, 1, "6 lines, 1 message");
    assert_eq!(acc.totals.output_tokens, 50, "not 300");
    assert_eq!(acc.buckets.len(), 1);
    assert_eq!(acc.buckets[0].output_tokens, 50);
}

#[test]
fn a_rewritten_usage_replaces_its_predecessor() {
    let mut acc = Accumulator::default();
    acc.feed(&assistant_line("m1", "2026-08-11T13:42:57.292Z", 50));
    acc.feed(&assistant_line("m1", "2026-08-11T13:42:58.000Z", 80));
    assert_eq!(acc.messages, 1);
    assert_eq!(acc.totals.output_tokens, 80, "replaced, not 130");
}

#[test]
fn distinct_messages_accumulate_and_newest_sets_model_and_timestamp() {
    let mut acc = Accumulator::default();
    acc.feed(&assistant_line("m1", "2026-08-11T13:42:00.000Z", 10));
    acc.feed(&assistant_line("m2", "2026-08-11T13:43:00.000Z", 20));
    assert_eq!(acc.messages, 2);
    assert_eq!(acc.totals.output_tokens, 30);
    assert_eq!(acc.totals.input_tokens, 4);
    assert_eq!(acc.last_timestamp, "2026-08-11T13:43:00.000Z");
    assert_eq!(acc.buckets.len(), 2, "different minutes, different buckets");
}

#[test]
fn buckets_cap_drops_the_oldest_minute() {
    let mut acc = Accumulator::default();
    for i in 0..(BUCKETS_MAX + 5) {
        let ts = format!("2026-08-11T10:{:02}:00.000Z", i);
        acc.feed(&assistant_line(&format!("m{i}"), &ts, 1));
    }
    assert_eq!(acc.buckets.len(), BUCKETS_MAX);
    assert_eq!(
        acc.buckets[0].minute, "2026-08-11T10:05",
        "the five oldest minutes fell off; totals keep them"
    );
    assert_eq!(acc.totals.output_tokens, (BUCKETS_MAX + 5) as u64);
}

#[test]
fn cursor_never_feeds_a_partial_line() {
    // One line split across two reads: byte-counting it as two lines
    // would register one malformed + one counted — the badge would
    // flicker on every poll that lands mid-line.
    let line = assistant_line("m1", "2026-08-11T13:42:57.292Z", 50) + "\n";
    let bytes = line.as_bytes();
    let mut acc = Accumulator::default();
    let mut cur = Cursor::default();
    let split = bytes.len() / 2;
    cur.feed_chunk(&bytes[..split], &mut acc);
    assert_eq!(acc.messages, 0, "half a line is not a line");
    assert_eq!(acc.malformed, 0, "…nor is it malformed yet");
    cur.feed_chunk(&bytes[split..], &mut acc);
    assert_eq!(acc.messages, 1);
    assert_eq!(acc.malformed, 0);
    assert_eq!(cur.offset(), bytes.len() as u64);
}

#[test]
fn watcher_tails_appends_and_resets_on_truncation() {
    let root = tempfile::tempdir().expect("tempdir");
    let proj = root.path().join("-opt-proj");
    std::fs::create_dir(&proj).unwrap();
    let file = proj.join("abcd-1234.jsonl");
    std::fs::write(
        &file,
        assistant_line("m1", "2026-08-11T13:42:00.000Z", 10) + "\n",
    )
    .unwrap();

    let mut w = Watcher::new(root.path().to_path_buf());
    let snap = w.snapshot();
    assert_eq!(snap.len(), 1);
    assert_eq!(snap[0].id, "abcd-1234");
    assert_eq!(snap[0].project, "-opt-proj");
    assert_eq!(snap[0].totals.output_tokens, 10);

    // Append: only the new bytes are read, the count grows.
    let mut cur = std::fs::read(&file).unwrap();
    cur.extend_from_slice((assistant_line("m2", "2026-08-11T13:43:00.000Z", 20) + "\n").as_bytes());
    std::fs::write(&file, cur).unwrap();
    let snap = w.snapshot();
    assert_eq!(snap[0].totals.output_tokens, 30);
    assert_eq!(snap[0].messages, 2);

    // Truncation (file rewritten shorter): counted state no longer
    // describes the file — start over rather than double-count.
    std::fs::write(
        &file,
        assistant_line("m9", "2026-08-11T13:44:00.000Z", 7) + "\n",
    )
    .unwrap();
    let snap = w.snapshot();
    assert_eq!(snap[0].totals.output_tokens, 7, "reset, not 37");
    assert_eq!(snap[0].messages, 1);
}

#[test]
fn watcher_ignores_non_jsonl_and_orders_newest_first() {
    let root = tempfile::tempdir().expect("tempdir");
    let proj = root.path().join("-opt-proj");
    std::fs::create_dir(&proj).unwrap();
    std::fs::write(proj.join("notes.txt"), "not a transcript").unwrap();
    std::fs::write(
        proj.join("old.jsonl"),
        assistant_line("m1", "2026-08-11T10:00:00.000Z", 1).to_string() + "\n",
    )
    .unwrap();
    std::fs::write(
        proj.join("new.jsonl"),
        assistant_line("m2", "2026-08-11T12:00:00.000Z", 2).to_string() + "\n",
    )
    .unwrap();
    let mut w = Watcher::new(root.path().to_path_buf());
    let snap = w.snapshot();
    assert_eq!(snap.len(), 2, "txt file ignored");
    assert_eq!(snap[0].id, "new", "newest transcript first");
    assert_eq!(snap[1].id, "old");
}
