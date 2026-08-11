//! Hooks-panel commands (design: app/design/claude-panel.md). Reads
//! Claude Code's OWN config under ~/.claude — the terminal protocol
//! stays workload-agnostic, and this module stays read-only: Claude
//! Code rewrites settings.json itself, and concurrent GUI writes are a
//! corruption hazard.

use std::io::{Read, Seek, SeekFrom};
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::SystemTime;

use hooks_model::{line_hash, parse_settings, HookLogEvent, HookRule};
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

// ---- hook-log tail + incremental chain verification ----
// (design: app/design/hook-log.md — the log is opt-in; this only reads)

/// How many recent events the panel shows. Older ones are still
/// verified (the chain runs over every line) — just not listed.
const LOG_EVENTS_SHOWN: usize = 50;

#[derive(Serialize, Clone)]
pub struct HookLogSnapshot {
    pub path: String,
    pub exists: bool,
    /// The most recent events, oldest→newest.
    pub events: Vec<HookLogEvent>,
    pub total: u64,
    pub malformed: u64,
    pub chain_ok: bool,
    /// 0-based line of the FIRST chain failure, when !chain_ok.
    pub break_at: Option<usize>,
}

/// Incremental tail state: only appended bytes are read and hashed per
/// poll (the claude-watch cursor pattern). A shrink resets everything —
/// counted state no longer describes the file.
#[derive(Default)]
struct LogTail {
    offset: u64,
    partial: Vec<u8>,
    /// Hash of the last complete line — what the next line's prev must
    /// equal ("GENESIS" before any line).
    expected: Option<String>,
    events: Vec<HookLogEvent>,
    lines_seen: usize,
    malformed: u64,
    break_at: Option<usize>,
}

impl LogTail {
    fn feed(&mut self, chunk: &[u8]) {
        self.offset += chunk.len() as u64;
        self.partial.extend_from_slice(chunk);
        while let Some(pos) = self.partial.iter().position(|&b| b == b'\n') {
            let raw: Vec<u8> = self.partial.drain(..=pos).collect();
            let line = String::from_utf8_lossy(&raw[..raw.len() - 1]).into_owned();
            self.push_line(&line);
        }
    }

    fn push_line(&mut self, line: &str) {
        let expected = self.expected.as_deref().unwrap_or("GENESIS");
        match serde_json::from_str::<serde_json::Value>(line) {
            Err(_) => {
                self.malformed += 1;
                if self.break_at.is_none() {
                    self.break_at = Some(self.lines_seen);
                }
            }
            Ok(v) => {
                let field = |k: &str| v.get(k).and_then(|x| x.as_str()).unwrap_or("").to_string();
                if field("prev") != expected && self.break_at.is_none() {
                    self.break_at = Some(self.lines_seen);
                }
                self.events.push(HookLogEvent {
                    ts: field("ts"),
                    hook: field("hook"),
                    event: field("event"),
                    tool: field("tool"),
                    decision: field("decision"),
                    reason: field("reason"),
                });
                if self.events.len() > LOG_EVENTS_SHOWN {
                    let drop = self.events.len() - LOG_EVENTS_SHOWN;
                    self.events.drain(..drop);
                }
            }
        }
        // The chain runs over raw bytes, JSON or not (hook-log.md).
        self.expected = Some(line_hash(line));
        self.lines_seen += 1;
    }
}

#[derive(Default)]
pub struct HookLogState(Mutex<LogTail>);

fn hook_log_path() -> Option<PathBuf> {
    std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".claude/hooks/hooks.log"))
}

fn log_snapshot_at(path: &PathBuf, tail: &mut LogTail) -> HookLogSnapshot {
    let display = path.display().to_string();
    let Ok(meta) = std::fs::metadata(path) else {
        *tail = LogTail::default(); // deleted: forget what we knew
        return HookLogSnapshot {
            path: display,
            exists: false,
            events: Vec::new(),
            total: 0,
            malformed: 0,
            chain_ok: true,
            break_at: None,
        };
    };
    if meta.len() < tail.offset {
        // Truncated or rewritten: re-verify from zero.
        *tail = LogTail::default();
    }
    if meta.len() > tail.offset {
        if let Ok(mut f) = std::fs::File::open(path) {
            if f.seek(SeekFrom::Start(tail.offset)).is_ok() {
                let mut buf = Vec::new();
                if f.read_to_end(&mut buf).is_ok() {
                    tail.feed(&buf);
                }
            }
        }
    }
    HookLogSnapshot {
        path: display,
        exists: true,
        events: tail.events.clone(),
        total: tail.lines_seen as u64,
        malformed: tail.malformed,
        chain_ok: tail.break_at.is_none(),
        break_at: tail.break_at,
    }
}

/// The security card's data: recent hook executions + chain verdict.
#[tauri::command]
pub fn hook_log_snapshot(state: tauri::State<'_, HookLogState>) -> Result<HookLogSnapshot, String> {
    let path = hook_log_path().ok_or("HOME is not set")?;
    Ok(log_snapshot_at(&path, &mut state.0.lock().unwrap()))
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

    fn log_line(prev: &str, reason: &str) -> String {
        format!(
            r#"{{"ts":"2026-08-11T12:00:00Z","hook":"g.sh","event":"PreToolUse","tool":"Bash","decision":"allow","reason":"{reason}","prev":"{prev}"}}"#
        )
    }

    #[test]
    fn log_tail_reads_incrementally_and_resets_on_truncation() {
        let dir = std::env::temp_dir().join(format!("hooklog-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("hooks.log");
        let l0 = log_line("GENESIS", "r0");
        let l1 = log_line(&line_hash(&l0), "r1");
        std::fs::write(&path, format!("{l0}\n")).unwrap();

        let mut tail = LogTail::default();
        let s = log_snapshot_at(&path, &mut tail);
        assert!(s.chain_ok);
        assert_eq!(s.total, 1);

        // Append: only new bytes are consumed, verdict stays green.
        std::fs::write(&path, format!("{l0}\n{l1}\n")).unwrap();
        let s = log_snapshot_at(&path, &mut tail);
        assert!(s.chain_ok);
        assert_eq!(s.total, 2);
        assert_eq!(s.events.len(), 2);

        // Tamper: rewrite line 0's reason without recomputing hashes.
        // The file KEEPS its length (reason r0→rX) so only re-reading
        // from zero can notice — which the shrink/rewrite path forces
        // here by truncating first.
        std::fs::write(&path, format!("{}\n", log_line("GENESIS", "rX"))).unwrap();
        let s = log_snapshot_at(&path, &mut tail);
        assert_eq!(s.total, 1, "shrink resets the tail");
        assert!(s.chain_ok, "single rewritten line is a valid 1-line chain");

        // A wrong prev on the appended line: chain breaks at line 1.
        std::fs::write(
            &path,
            format!(
                "{}\n{}\n",
                log_line("GENESIS", "rX"),
                log_line("deadbeef", "r2")
            ),
        )
        .unwrap();
        let s = log_snapshot_at(&path, &mut tail);
        assert!(!s.chain_ok);
        assert_eq!(s.break_at, Some(1));
        // Deleted: honest absent state, tail forgotten.
        std::fs::remove_file(&path).unwrap();
        let s = log_snapshot_at(&path, &mut tail);
        assert!(!s.exists);
        assert!(s.chain_ok);
        std::fs::remove_dir_all(&dir).ok();
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
