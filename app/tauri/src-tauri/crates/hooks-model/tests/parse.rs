//! Fixture tables for parse_settings. Shapes mirror the schema observed
//! in a real ~/.claude/settings.json (2026-08) plus the documented
//! superset; content is synthetic.

use hooks_model::{parse_settings, HookRule};

/// The real-machine shape: one PreToolUse matcher group, TWO commands,
/// no timeouts, sibling top-level keys that must be ignored.
const REAL_SHAPE: &str = r#"{
  "model": "opusplan",
  "env": {"X": "1"},
  "permissions": {"allow": ["Bash(ls:*)"]},
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          {"type": "command", "command": "/home/u/.claude/hooks/block-git-push.sh"},
          {"type": "command", "command": "/home/u/.claude/hooks/check-redaction.sh"}
        ]
      }
    ]
  }
}"#;

#[test]
fn real_shape_yields_one_row_per_command_in_file_order() {
    let cfg = parse_settings(REAL_SHAPE);
    assert_eq!(cfg.malformed, 0);
    assert_eq!(
        cfg.rules,
        vec![
            HookRule {
                event: "PreToolUse".into(),
                matcher: "Bash".into(),
                command: "/home/u/.claude/hooks/block-git-push.sh".into(),
                timeout: None,
            },
            HookRule {
                event: "PreToolUse".into(),
                matcher: "Bash".into(),
                command: "/home/u/.claude/hooks/check-redaction.sh".into(),
                timeout: None,
            },
        ]
    );
}

#[test]
fn matcherless_events_normalize_to_star_and_timeout_is_read() {
    // Stop/UserPromptSubmit legitimately carry no matcher.
    let cfg = parse_settings(
        r#"{"hooks": {"Stop": [{"hooks": [
            {"type": "command", "command": "notify.sh", "timeout": 30}
        ]}]}}"#,
    );
    assert_eq!(cfg.malformed, 0);
    assert_eq!(cfg.rules.len(), 1);
    assert_eq!(cfg.rules[0].matcher, "*");
    assert_eq!(cfg.rules[0].timeout, Some(30));
}

#[test]
fn unknown_event_names_still_display() {
    // Future Claude Code events must not vanish from the table.
    let cfg = parse_settings(
        r#"{"hooks": {"SomeFutureEvent": [{"matcher": "Edit", "hooks": [
            {"type": "command", "command": "x.sh"}
        ]}]}}"#,
    );
    assert_eq!(cfg.rules[0].event, "SomeFutureEvent");
}

#[test]
fn no_hooks_key_is_normal_not_malformed() {
    let cfg = parse_settings(r#"{"model": "opusplan"}"#);
    assert_eq!(cfg.rules.len(), 0);
    assert_eq!(cfg.malformed, 0, "unconfigured is a state, not an error");
}

#[test]
fn garbage_file_is_one_malformed_unit() {
    let cfg = parse_settings("not json at all");
    assert_eq!(cfg.rules.len(), 0);
    assert_eq!(cfg.malformed, 1);
}

#[test]
fn wrong_shapes_are_counted_and_skipped_never_fatal() {
    let cfg = parse_settings(
        r#"{"hooks": {
            "PreToolUse": "not an array",
            "PostToolUse": [
                {"matcher": "Bash"},
                {"matcher": "Edit", "hooks": [
                    {"type": "prompt", "command": "not-a-command-type.sh"},
                    {"type": "command"},
                    {"type": "command", "command": "good.sh", "timeout": "soon"}
                ]}
            ]
        }}"#,
    );
    // not-an-array event + group without hooks + non-command type +
    // command-less entry = 4 malformed; the good row still comes out,
    // its unreadable timeout degrading to None rather than killing it.
    assert_eq!(cfg.malformed, 4);
    assert_eq!(cfg.rules.len(), 1);
    assert_eq!(cfg.rules[0].command, "good.sh");
    assert_eq!(cfg.rules[0].timeout, None);
}

#[test]
fn hooks_present_but_not_an_object_is_malformed() {
    let cfg = parse_settings(r#"{"hooks": []}"#);
    assert_eq!(cfg.malformed, 1);
    assert_eq!(cfg.rules.len(), 0);
}
