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
fn a_rewrite_in_a_later_minute_credits_the_minute_it_was_counted_in() {
    // The rewrite above happens inside one minute, which is the easy case.
    // A finalized usage can land after the minute boundary instead, and
    // then the retraction belongs to the minute the tokens were ADDED to,
    // not to the minute the rewrite arrived in. Measured on one machine:
    // the 85 transcripts this watcher can read contain 2 rewrites, 1 of
    // them cross-minute; the full tree (1,857 files, including the
    // subagent/workflow transcripts list_active does not yet descend
    // into) contains 1,079 cross-minute rewrites.
    let mut acc = Accumulator::default();
    acc.feed(&assistant_line("m1", "2026-08-11T13:42:59.000Z", 50));
    acc.feed(&assistant_line("m2", "2026-08-11T13:43:01.000Z", 7));
    acc.feed(&assistant_line("m1", "2026-08-11T13:43:02.000Z", 80));

    assert_eq!(acc.messages, 2, "m1 rewritten, m2 new");
    assert_eq!(acc.totals.output_tokens, 87, "80 replaces 50, plus m2's 7");
    let minute = |m: &str| {
        acc.buckets
            .iter()
            .find(|b| b.minute == m)
            .map(|b| b.output_tokens)
    };
    assert_eq!(
        minute("2026-08-11T13:42"),
        Some(0),
        "the retracted 50 must leave the minute it was counted in"
    );
    assert_eq!(
        minute("2026-08-11T13:43"),
        Some(87),
        "m2's 7 plus the rewritten 80 — and nothing subtracted here"
    );
    // The window is 30 buckets, so within it the bars must sum to the
    // total. This is the property the wrong minute breaks.
    assert_eq!(
        acc.buckets.iter().map(|b| b.output_tokens).sum::<u64>(),
        acc.totals.output_tokens
    );
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
fn a_read_budget_spreads_history_across_calls_and_says_so() {
    // The first snapshot over a machine's history measured 131.4 MB /
    // ~81 s as ONE synchronous call. Under a budget the same bytes are
    // read a slice per call: every capped call must (a) stop at the
    // budget, (b) admit what it skipped via pending_bytes, (c) resume
    // where it stopped, and (d) converge on exactly the numbers an
    // unbudgeted read produces — nothing dropped at the seams, even
    // when the cap lands mid-line.
    let root = tempfile::tempdir().expect("tempdir");
    let proj = root.path().join("-opt-proj");
    std::fs::create_dir(&proj).unwrap();
    let mut content = String::new();
    for i in 0..200 {
        let ts = format!("2026-08-11T10:{:02}:{:02}.000Z", i / 60, i % 60);
        content.push_str(&assistant_line(&format!("m{i}"), &ts, 3));
        content.push('\n');
    }
    let total_len = content.len() as u64;
    std::fs::write(proj.join("big.jsonl"), &content).unwrap();

    let mut w = Watcher::new(root.path().to_path_buf());
    let budget = total_len / 7; // guaranteed to cut lines mid-way
    let first = w.snapshot_with_budget(budget);
    assert_eq!(first.len(), 1, "a still-loading transcript is a row");
    assert!(
        first[0].messages < 200,
        "the budget must actually stop the read ({} of 200)",
        first[0].messages
    );
    assert!(
        first[0].pending_bytes > 0,
        "a partial row must admit it is partial"
    );

    let mut calls = 1;
    loop {
        let snap = w.snapshot_with_budget(budget);
        calls += 1;
        assert!(calls < 20, "never converged");
        if snap[0].pending_bytes == 0 {
            assert_eq!(snap[0].messages, 200, "everything counted, once");
            assert_eq!(snap[0].totals.output_tokens, 600);
            break;
        }
    }

    // Steady state after convergence: appended bytes still arrive.
    let mut cur = std::fs::read(proj.join("big.jsonl")).unwrap();
    cur.extend_from_slice(
        (assistant_line("m-new", "2026-08-11T11:00:00.000Z", 5) + "\n").as_bytes(),
    );
    std::fs::write(proj.join("big.jsonl"), cur).unwrap();
    let snap = w.snapshot_with_budget(budget);
    assert_eq!(snap[0].messages, 201);
    assert_eq!(snap[0].totals.output_tokens, 605);
    assert_eq!(snap[0].pending_bytes, 0);
}

#[test]
fn the_budget_is_shared_across_files_not_granted_per_file() {
    // Two transcripts, one call, budget = half their combined bytes.
    // The call must stop at the budget IN TOTAL — a per-file grant would
    // read both files whole and the panel's "still reading" honesty
    // would never trigger on exactly the machine that needs it (many
    // active transcripts). Read sizes are exact (read_slice takes
    // min(want, remaining) bytes), so the leftover is exact too.
    let root = tempfile::tempdir().expect("tempdir");
    let proj = root.path().join("-opt-proj");
    std::fs::create_dir(&proj).unwrap();
    let mut total: u64 = 0;
    for name in ["a.jsonl", "b.jsonl"] {
        let mut content = String::new();
        for i in 0..100 {
            let ts = format!("2026-08-11T10:{:02}:{:02}.000Z", i / 60, i % 60);
            content.push_str(&assistant_line(&format!("{name}-m{i}"), &ts, 2));
            content.push('\n');
        }
        total += content.len() as u64;
        std::fs::write(proj.join(name), &content).unwrap();
    }

    let mut w = Watcher::new(root.path().to_path_buf());
    let budget = total / 2;
    let first = w.snapshot_with_budget(budget);
    let pending: u64 = first.iter().map(|r| r.pending_bytes).sum();
    assert_eq!(
        pending,
        total - budget,
        "one call reads budget bytes in total; everything else is owed"
    );

    let mut calls = 1;
    loop {
        let snap = w.snapshot_with_budget(budget);
        calls += 1;
        assert!(calls < 10, "never converged");
        if snap.iter().all(|r| r.pending_bytes == 0) {
            let msgs: u64 = snap.iter().map(|r| r.messages).sum();
            let out: u64 = snap.iter().map(|r| r.totals.output_tokens).sum();
            assert_eq!(msgs, 200, "both files fully counted, nothing twice");
            assert_eq!(out, 400);
            break;
        }
    }
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
