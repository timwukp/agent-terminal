//! Token-usage snapshot for the Claude panel (design:
//! app/design/claude-panel.md). All numbers come from Claude Code's own
//! transcript files under `~/.claude/projects` — the terminal protocol
//! stays workload-agnostic, so nothing here touches the daemon.

use std::path::PathBuf;
use std::sync::Mutex;

use claude_watch::{TranscriptUsage, Watcher};

pub struct UsageState(Mutex<Option<Watcher>>);

impl Default for UsageState {
    fn default() -> Self {
        Self(Mutex::new(None))
    }
}

fn projects_root() -> Option<PathBuf> {
    std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".claude").join("projects"))
}

/// Current per-transcript token totals, newest activity first. Each call
/// stats the active files and reads only appended bytes — capped at
/// claude-watch's `SNAPSHOT_READ_BUDGET` per call — so a call costs what
/// changed, never more than the cap, and rows still catching up say so
/// via `pending_bytes`.
///
/// Called twice: once by the panel when it mounts, so it paints without
/// waiting for a tick, and then once per tick by the push stream
/// (panels.rs), which sends the result on only when it differs.
///
/// `(async)` because a sync command runs inline in wry's IPC callback —
/// on macOS the main thread — and the mount-time call over a machine
/// with months of transcripts measured 81 s of parsing: the window froze
/// for all of it. The async execution context moves the body to a
/// worker; the budget above bounds how long any one call can take there.
#[tauri::command(async)]
pub fn usage_snapshot(state: tauri::State<'_, UsageState>) -> Result<Vec<TranscriptUsage>, String> {
    let mut guard = state.0.lock().unwrap();
    let watcher = match guard.as_mut() {
        Some(w) => w,
        None => {
            let root = projects_root().ok_or("HOME is not set")?;
            guard.insert(Watcher::new(root))
        }
    };
    Ok(watcher.snapshot())
}
