//! Read-only model of Claude Code's hooks configuration
//! (design: app/design/claude-panel.md). Lands in PR8; PR1 ships the
//! shared row type so the crate compiles.

/// One row of the hooks table: an event/matcher/command triple as read
/// from settings.json. Read-only in v1 by design — Claude Code itself
/// rewrites that file, and concurrent GUI writes are a corruption hazard.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HookRule {
    pub event: String,
    pub matcher: String,
    pub command: String,
}
