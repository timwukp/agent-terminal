//! Hooks-panel commands (design: app/design/claude-panel.md). Reads
//! Claude Code's OWN config under ~/.claude — the terminal protocol
//! stays workload-agnostic, and this module stays read-only: Claude
//! Code rewrites settings.json itself, and concurrent GUI writes are a
//! corruption hazard.

use std::path::PathBuf;
use std::sync::Mutex;
use std::time::SystemTime;

use hooks_model::{parse_settings, HookRule};
use serde::Serialize;

#[derive(Serialize, Clone)]
pub struct HooksSnapshot {
    /// The file the rules came from, so the panel can name its source.
    pub path: String,
    pub exists: bool,
    pub rules: Vec<HookRule>,
    pub malformed: u64,
}

struct Cached {
    stamp: (SystemTime, u64),
    snapshot: HooksSnapshot,
}

#[derive(Default)]
pub struct HooksState(Mutex<Option<Cached>>);

fn settings_path() -> Option<PathBuf> {
    std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".claude").join("settings.json"))
}

/// The cacheable core, taking the cache slot directly so tests can
/// drive it without a tauri State.
fn snapshot_at(path: &PathBuf, cache: &mut Option<Cached>) -> Result<HooksSnapshot, String> {
    let display = path.display().to_string();
    let Ok(meta) = std::fs::metadata(path) else {
        return Ok(HooksSnapshot {
            path: display,
            exists: false,
            rules: Vec::new(),
            malformed: 0,
        });
    };
    let stamp = (
        meta.modified().unwrap_or(SystemTime::UNIX_EPOCH),
        meta.len(),
    );
    if let Some(c) = cache.as_ref() {
        if c.stamp == stamp {
            return Ok(c.snapshot.clone());
        }
    }
    let text = std::fs::read_to_string(path).map_err(|e| e.to_string())?;
    let cfg = parse_settings(&text);
    let snapshot = HooksSnapshot {
        path: display,
        exists: true,
        rules: cfg.rules,
        malformed: cfg.malformed,
    };
    *cache = Some(Cached {
        stamp,
        snapshot: snapshot.clone(),
    });
    Ok(snapshot)
}

/// The viewer gate: only a command string that EXACTLY matches a rule
/// in the current snapshot is honored, and only when it names an
/// existing regular file. Anything else is refused — a webview that can
/// read arbitrary paths through a "show me the script" command is an
/// arbitrary-file-read hole, not a viewer. Inline commands (not a file)
/// render as-is on the JS side and never reach here.
fn read_script_gated(cache: &Option<Cached>, command: &str) -> Result<String, String> {
    let known = cache
        .as_ref()
        .map(|c| c.snapshot.rules.iter().any(|r| r.command == command))
        .unwrap_or(false);
    if !known {
        return Err("not a configured hook command".into());
    }
    let path = PathBuf::from(command);
    if !path.is_file() {
        return Err("hook command is not a file on disk (inline command?)".into());
    }
    std::fs::read_to_string(&path).map_err(|e| e.to_string())
}

/// Current hooks rules. Polled at the sidebar cadence while the tab is
/// open; the (mtime, len) cache means a poll normally costs one stat.
#[tauri::command]
pub fn hooks_snapshot(state: tauri::State<'_, HooksState>) -> Result<HooksSnapshot, String> {
    let path = settings_path().ok_or("HOME is not set")?;
    snapshot_at(&path, &mut state.0.lock().unwrap())
}

/// Source of one hook script, for the read-only viewer (gate above).
#[tauri::command]
pub fn read_hook_script(
    state: tauri::State<'_, HooksState>,
    command: String,
) -> Result<String, String> {
    read_script_gated(&state.0.lock().unwrap(), &command)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write(path: &PathBuf, body: &str) {
        std::fs::write(path, body).unwrap();
    }

    const ONE_HOOK: &str = r#"{"hooks":{"PreToolUse":[{"matcher":"Bash","hooks":[
        {"type":"command","command":"__SCRIPT__"}]}]}}"#;

    #[test]
    fn cache_refreshes_when_the_file_changes_and_not_when_it_does_not() {
        let dir = std::env::temp_dir().join(format!("hooks-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("settings.json");
        write(&path, &ONE_HOOK.replace("__SCRIPT__", "/tmp/a.sh"));
        let mut cache = None;
        let s1 = snapshot_at(&path, &mut cache).unwrap();
        assert_eq!(s1.rules.len(), 1);
        // Unchanged file: served from cache (same stamp).
        let s2 = snapshot_at(&path, &mut cache).unwrap();
        assert_eq!(s2.rules[0].command, "/tmp/a.sh");
        // Changed file (different length → different stamp even when
        // mtime granularity is coarse): must re-parse, not serve stale.
        write(&path, &ONE_HOOK.replace("__SCRIPT__", "/tmp/bbbb.sh"));
        let s3 = snapshot_at(&path, &mut cache).unwrap();
        assert_eq!(s3.rules[0].command, "/tmp/bbbb.sh", "stale cache served");
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn missing_file_is_an_honest_absent_state() {
        let mut cache = None;
        let s = snapshot_at(&PathBuf::from("/nonexistent/settings.json"), &mut cache).unwrap();
        assert!(!s.exists);
        assert!(s.rules.is_empty());
    }

    #[test]
    fn script_gate_refuses_everything_not_in_the_snapshot() {
        let dir = std::env::temp_dir().join(format!("hooks-gate-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let script = dir.join("guard.sh");
        write(&script, "#!/bin/sh\nexit 2\n");
        let secret = dir.join("secret.txt");
        write(&secret, "credentials");
        let settings = dir.join("settings.json");
        write(
            &settings,
            &ONE_HOOK.replace("__SCRIPT__", &script.display().to_string()),
        );
        let mut cache = None;
        snapshot_at(&settings, &mut cache).unwrap();

        // Configured script: served.
        let src = read_script_gated(&cache, &script.display().to_string()).unwrap();
        assert!(src.contains("exit 2"));
        // A real file that is NOT a configured hook: refused. This is
        // the arbitrary-file-read hole the gate exists to close.
        assert!(read_script_gated(&cache, &secret.display().to_string()).is_err());
        // No snapshot yet: refused.
        assert!(read_script_gated(&None, &script.display().to_string()).is_err());
        std::fs::remove_dir_all(&dir).ok();
    }
}
