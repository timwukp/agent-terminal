//! Read-only model of Claude Code's hooks configuration
//! (design: app/design/claude-panel.md).
//!
//! Parses the `hooks` section of `~/.claude/settings.json`. The schema
//! is Claude Code's, not ours, and not a stable contract — so parsing
//! follows the claude-watch discipline: unknown fields ignored,
//! wrong-shaped entries COUNTED (the panel shows "N unparsed") but
//! never fatal, and a missing `hooks` key is a normal state, not an
//! anomaly. Events iterate in serde_json's default map order
//! (alphabetical) — NOT the file's: enabling `preserve_order` here
//! would feature-unify onto every workspace crate and reorder
//! at-proto's golden vector bytes. Within one event, array order (the
//! order that actually encodes precedence) is preserved.

use serde::Serialize;
use sha2::{Digest, Sha256};

/// One row of the hooks table. One row per COMMAND, not per matcher
/// group: the real-world norm is one group carrying several commands
/// (this machine runs two guards on PreToolUse/Bash). Read-only in v1
/// by design — Claude Code itself rewrites settings.json, and
/// concurrent GUI writes are a corruption hazard.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct HookRule {
    pub event: String,
    /// Tool-name pattern. Legitimately absent for events like Stop or
    /// UserPromptSubmit — normalized to "*" (matches the semantics:
    /// the hook fires for every occurrence of the event).
    pub matcher: String,
    pub command: String,
    pub timeout: Option<u64>,
}

#[derive(Debug, Default, Serialize)]
pub struct HooksConfig {
    pub rules: Vec<HookRule>,
    /// Entries the parser could not read. Shown as a badge, so the
    /// table never silently claims completeness it does not have.
    pub malformed: u64,
}

/// Parse the full settings.json text. Never errors: garbage in →
/// `malformed` counts up and whatever was readable comes out.
pub fn parse_settings(json: &str) -> HooksConfig {
    let mut out = HooksConfig::default();
    let Ok(root) = serde_json::from_str::<serde_json::Value>(json) else {
        out.malformed = 1; // the whole file, as one unreadable unit
        return out;
    };
    // No hooks key (or a non-object root): nothing configured — normal.
    let Some(events) = root.get("hooks").and_then(|h| h.as_object()) else {
        if root.get("hooks").is_some() {
            out.malformed += 1; // present but not an object
        }
        return out;
    };
    for (event, groups) in events {
        let Some(groups) = groups.as_array() else {
            out.malformed += 1;
            continue;
        };
        for group in groups {
            // matcher is optional by schema; absent ≠ malformed.
            let matcher = group
                .get("matcher")
                .and_then(|m| m.as_str())
                .unwrap_or("*")
                .to_string();
            let Some(hooks) = group.get("hooks").and_then(|h| h.as_array()) else {
                out.malformed += 1;
                continue;
            };
            for hook in hooks {
                let is_command = hook.get("type").and_then(|t| t.as_str()) == Some("command");
                let command = hook.get("command").and_then(|c| c.as_str());
                match (is_command, command) {
                    (true, Some(command)) => out.rules.push(HookRule {
                        event: event.clone(),
                        matcher: matcher.clone(),
                        command: command.to_string(),
                        timeout: hook.get("timeout").and_then(|t| t.as_u64()),
                    }),
                    // Unknown type or missing command: count it — a row
                    // we cannot render is a row the badge must admit to.
                    _ => out.malformed += 1,
                }
            }
        }
    }
    out
}

// ---- hook-log chain verification (design: app/design/hook-log.md) ----

/// One displayed hook execution. All fields are free text the GUI
/// renders; `decision` gets a color, never semantics.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct HookLogEvent {
    pub ts: String,
    pub hook: String,
    pub event: String,
    pub tool: String,
    pub decision: String,
    pub reason: String,
}

#[derive(Debug, Default, Serialize)]
pub struct ChainReport {
    pub events: Vec<HookLogEvent>,
    pub malformed: u64,
    /// First 0-based line index where the chain fails — a wrong `prev`,
    /// or a line that is not JSON. None = intact. Everything AFTER a
    /// break still parses and displays: a break is a fact about the
    /// file, not a reason to hide history.
    pub break_at: Option<usize>,
}

/// SHA-256 of one raw line (no trailing newline), lowercase hex — the
/// value the NEXT line's `prev` must carry.
pub fn line_hash(line: &str) -> String {
    let mut h = Sha256::new();
    h.update(line.as_bytes());
    format!("{:x}", h.finalize())
}

/// Verify a hook log: every line's `prev` must equal the hash of the
/// line before it ("GENESIS" for line 0). Lenient like every parser in
/// this crate — a bad line is counted and breaks the chain, but
/// verification and display continue past it.
pub fn verify_chain(lines: &[&str]) -> ChainReport {
    let mut out = ChainReport::default();
    let mut expected = "GENESIS".to_string();
    for (i, line) in lines.iter().enumerate() {
        let parsed = serde_json::from_str::<serde_json::Value>(line).ok();
        let Some(v) = parsed else {
            out.malformed += 1;
            if out.break_at.is_none() {
                out.break_at = Some(i);
            }
            // The next line's prev is the hash of these RAW bytes
            // regardless of their JSON-ness — the chain runs over
            // bytes, so verification continues through a bad line.
            expected = line_hash(line);
            continue;
        };
        let field = |k: &str| v.get(k).and_then(|x| x.as_str()).unwrap_or("").to_string();
        if field("prev") != expected && out.break_at.is_none() {
            out.break_at = Some(i);
        }
        out.events.push(HookLogEvent {
            ts: field("ts"),
            hook: field("hook"),
            event: field("event"),
            tool: field("tool"),
            decision: field("decision"),
            reason: field("reason"),
        });
        expected = line_hash(line);
    }
    out
}
