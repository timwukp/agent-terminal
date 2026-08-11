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
