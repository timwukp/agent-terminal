//! Hooks-panel commands (design: app/design/claude-panel.md). Reads
//! Claude Code's OWN config under ~/.claude — the terminal protocol
//! stays workload-agnostic, and this module stays read-only: Claude
//! Code rewrites settings.json itself, and concurrent GUI writes are a
//! corruption hazard.

use std::io::{Read, Seek, SeekFrom};
use std::os::unix::fs::MetadataExt;
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

/// Ceiling on what the viewer will render from one hook script. A hook
/// command is a script a person wrote, and 1 MiB is ~20k lines of shell;
/// the cap is here because the read is triggered by a click in the
/// webview and the result crosses the IPC boundary as a single JSON
/// string, so the file's size is copied several times over before
/// anything can be displayed.
const SCRIPT_READ_MAX: u64 = 1 << 20;

/// Appended to a truncated script, inside the returned text. The panel
/// renders exactly what it is handed, so a silent truncation would read
/// as "this is the whole script" — the worst available outcome for a
/// viewer whose entire job is auditing what a hook does.
const SCRIPT_TRUNCATED: &str = "\n\n\
    ⋯ truncated: this hook script is larger than 1 MiB, and only the first 1 MiB is \
    shown here. Read the file directly to audit the rest.\n";

/// The same file, not merely the same path. (device, inode) is the
/// identity an open fd carries and a path does not, which is what makes
/// it usable as an after-the-fact check on what a path resolved to.
fn same_file(a: &std::fs::Metadata, b: &std::fs::Metadata) -> bool {
    (a.dev(), a.ino()) == (b.dev(), b.ino())
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
    // lstat, not stat: `Path::is_file` goes through `fs::metadata` and so
    // answers about the SYMLINK TARGET. The matched-command check bounds
    // which path is read, but not what that path currently points at, and
    // a hook command normally lives somewhere far more writable than
    // ~/.claude — /tmp/guard.sh, or a script inside a checked-out repo.
    // Replace it with a symlink to ~/.ssh/id_rsa and the panel renders
    // the key the next time its owner clicks the row; the attacker never
    // needs read access to the target, because the viewer supplies it.
    // Requiring a REGULAR file also rules out a directory, a device node,
    // and a FIFO — the last of which would otherwise turn one click into
    // a read that never returns while holding the HooksState lock.
    let lmeta = std::fs::symlink_metadata(&path)
        .map_err(|_| "hook command is not a file on disk (inline command?)".to_string())?;
    if !lmeta.file_type().is_file() {
        return Err("hook command is not a regular file (symlink, fifo or device?)".into());
    }
    // Whoever can plant the symlink can also plant it in the window
    // between the lstat above and this open, which follows symlinks. The
    // fd's own identity settles it: same device and inode means the bytes
    // below came from the file that was checked, and nothing else.
    let f = std::fs::File::open(&path).map_err(|e| e.to_string())?;
    let fmeta = f.metadata().map_err(|e| e.to_string())?;
    if !same_file(&lmeta, &fmeta) {
        return Err("hook command changed while being opened".into());
    }
    read_capped(f).map(|(text, _read)| text)
}

/// At most `SCRIPT_READ_MAX` bytes of `f` as text, plus a count of how
/// many bytes were actually consumed.
///
/// The count is returned because it is the only way to tell the fix from
/// something that merely looks like it: reading the whole file and then
/// trimming the string produces byte-identical output while still paying
/// for the file's full size in memory, which is the cost being refused.
fn read_capped(f: std::fs::File) -> Result<(String, u64), String> {
    // One byte past the cap, so a file exactly at the cap is not reported
    // as truncated and one over it is detected without reading the rest.
    let mut buf = Vec::new();
    f.take(SCRIPT_READ_MAX + 1)
        .read_to_end(&mut buf)
        .map_err(|e| e.to_string())?;
    let read = buf.len() as u64;
    if read > SCRIPT_READ_MAX {
        buf.truncate(SCRIPT_READ_MAX as usize);
        // Lossy only on this path: the cut can land mid-codepoint, which
        // is an artifact of the cap rather than something wrong with the
        // file. An untruncated script that is not UTF-8 is still an error.
        let mut text = String::from_utf8_lossy(&buf).into_owned();
        text.push_str(SCRIPT_TRUNCATED);
        return Ok((text, read));
    }
    let text = String::from_utf8(buf).map_err(|_| "hook command is not UTF-8 text".to_string())?;
    Ok((text, read))
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

/// Ceiling on how many appended bytes ONE poll consumes. Nothing is
/// skipped: the offset advances only by what was read, so a backlog is
/// verified over the next few polls instead of inside a single one. The
/// unbounded version read the whole delta into one Vec and hashed every
/// line of it on a blocking command call — fine after two seconds of
/// hooks, not after an unattended week of them.
const LOG_DELTA_MAX: u64 = 1 << 20;

/// Ceiling on ONE unterminated line. The per-poll cap above bounds work
/// per poll but not memory: bytes with no newline in them stay in
/// `partial` and accumulate across polls, a megabyte at a time, for as
/// long as the writer withholds the newline. A hook event is one JSON
/// object on one line; past this it is not one, so it is counted
/// malformed and its bytes are dropped rather than buffered — and
/// because a dropped line cannot be hashed, the chain verdict stays
/// broken, which is the honest report.
const LOG_LINE_MAX: usize = 1 << 20;

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
    /// Set while discarding the remainder of a line that exceeded
    /// `LOG_LINE_MAX`; cleared by the newline that ends it, which is the
    /// only place the reader can resynchronize.
    oversize: bool,
}

impl LogTail {
    fn feed(&mut self, chunk: &[u8]) {
        self.offset += chunk.len() as u64;
        self.partial.extend_from_slice(chunk);
        while let Some(pos) = self.partial.iter().position(|&b| b == b'\n') {
            let raw: Vec<u8> = self.partial.drain(..=pos).collect();
            if self.oversize {
                self.oversize = false; // the tail of a line already refused
                continue;
            }
            let line = String::from_utf8_lossy(&raw[..raw.len() - 1]).into_owned();
            self.push_line(&line);
        }
        // Whatever is left holds no newline at all. Buffering it without a
        // bound is the leak the per-poll cap does not cover. Reaching here
        // with `oversize` still set means this whole chunk was more of the
        // refused line, since only a newline clears the flag.
        if self.oversize {
            self.partial.clear();
        } else if self.partial.len() > LOG_LINE_MAX {
            self.oversize = true;
            self.malformed += 1;
            if self.break_at.is_none() {
                self.break_at = Some(self.lines_seen);
            }
            self.lines_seen += 1;
            self.partial.clear();
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
                // Bounded per poll. `total` therefore describes what has
                // been VERIFIED, not what the file holds, until the polls
                // catch up — which is the number the panel should show
                // anyway, since an unread line has no verdict yet.
                if f.take(LOG_DELTA_MAX).read_to_end(&mut buf).is_ok() {
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

    /// Configure a single hook whose command is `script`, and return the
    /// warmed cache the gate reads from.
    fn cache_for(dir: &std::path::Path, script: &std::path::Path) -> Option<Cached> {
        let settings = dir.join("settings.json");
        write(
            &settings,
            &ONE_HOOK.replace("__SCRIPT__", &script.display().to_string()),
        );
        let mut cache = None;
        snapshot_at(&settings, &mut cache).unwrap();
        cache
    }

    #[test]
    fn script_gate_refuses_a_symlink_even_when_it_is_the_configured_command() {
        let dir = std::env::temp_dir().join(format!("hooks-symlink-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        // Stands in for ~/.ssh/id_rsa: something the viewer must never
        // render, that the gate would happily read if it followed links.
        // The marker deliberately does NOT spell a real key header — the
        // repo's redaction gate scans for one, and a fixture that trips a
        // security gate on every push teaches people to wave it through.
        const SECRET: &str = "KEY-MATERIAL-STANDIN-must-never-be-rendered";
        let secret = dir.join("id_rsa");
        write(&secret, &format!("{SECRET}\n"));
        // The configured hook command IS the link, so the matched-command
        // check passes: this is exactly the case the exact-match gate
        // cannot see, because it bounds the path, not the path's target.
        let link = dir.join("guard.sh");
        std::os::unix::fs::symlink(&secret, &link).unwrap();
        let cache = cache_for(&dir, &link);

        let err = read_script_gated(&cache, &link.display().to_string()).unwrap_err();
        assert!(
            err.contains("regular file"),
            "a symlink to a private key must be refused as a non-regular file, got: {err}"
        );
        assert!(
            !err.contains(SECRET),
            "the refusal must not carry the target's bytes either, got: {err}"
        );

        // Discrimination control: the SAME configured path, now a real
        // script, is still served. Without it, a gate that refused
        // everything would pass the assertion above.
        std::fs::remove_file(&link).unwrap();
        write(&link, "#!/bin/sh\nexit 2\n");
        let src = read_script_gated(&cache, &link.display().to_string()).unwrap();
        assert!(
            src.contains("exit 2"),
            "a real script at the same path must be served"
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    /// The lstat→open window is only closed if this predicate really
    /// compares identities; the race itself cannot be scheduled by a test,
    /// but the symlink-vs-target case below is exactly what an attacker
    /// would win it with, and it is deterministic.
    #[test]
    fn same_file_compares_identity_not_paths() {
        let dir = std::env::temp_dir().join(format!("hooks-ident-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let a = dir.join("a");
        write(&a, "a");
        let b = dir.join("b");
        write(&b, "b");
        let ma = std::fs::symlink_metadata(&a).unwrap();
        let fa = std::fs::File::open(&a).unwrap().metadata().unwrap();
        assert!(
            same_file(&ma, &fa),
            "one file through lstat and fstat must match"
        );
        assert!(
            !same_file(&ma, &std::fs::symlink_metadata(&b).unwrap()),
            "two different files must not compare equal"
        );
        let link = dir.join("l");
        std::os::unix::fs::symlink(&b, &link).unwrap();
        assert!(
            !same_file(
                &std::fs::symlink_metadata(&link).unwrap(),
                &std::fs::File::open(&link).unwrap().metadata().unwrap()
            ),
            "an opened symlink must not match the link's own inode — that mismatch IS the check"
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn script_gate_truncates_an_oversized_script_and_says_so() {
        let dir = std::env::temp_dir().join(format!("hooks-big-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let script = dir.join("big.sh");
        // One byte over the cap, with a marker in the byte that falls just
        // outside it: the marker's absence is what proves the read stopped
        // rather than merely that the notice was appended.
        let mut body = vec![b'x'; SCRIPT_READ_MAX as usize];
        body.extend_from_slice(b"MARKER-PAST-THE-CAP");
        std::fs::write(&script, &body).unwrap();
        let cache = cache_for(&dir, &script);

        let src = read_script_gated(&cache, &script.display().to_string()).unwrap();
        assert!(
            !src.contains("MARKER-PAST-THE-CAP"),
            "the read was not capped: {} bytes came back",
            src.len()
        );
        assert!(
            src.contains("truncated"),
            "a truncated script must say so, not look complete"
        );
        assert_eq!(
            src.len(),
            SCRIPT_READ_MAX as usize + SCRIPT_TRUNCATED.len(),
            "exactly the cap plus the notice"
        );
        // The cap has to bound the READ, and the string above cannot show
        // that: read-then-trim returns the same bytes while still holding
        // the whole file in memory first.
        let (_, read) = read_capped(std::fs::File::open(&script).unwrap()).unwrap();
        assert_eq!(
            read,
            SCRIPT_READ_MAX + 1,
            "the read itself was never capped"
        );

        // Control: a script exactly AT the cap is complete, and carries no
        // notice — an off-by-one here would libel every large-but-fine file.
        std::fs::write(&script, vec![b'y'; SCRIPT_READ_MAX as usize]).unwrap();
        let src = read_script_gated(&cache, &script.display().to_string()).unwrap();
        assert_eq!(src.len(), SCRIPT_READ_MAX as usize);
        assert!(!src.contains("truncated"));
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn log_tail_bounds_one_poll_without_losing_lines() {
        let dir = std::env::temp_dir().join(format!("hooklog-delta-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("hooks.log");

        // A valid chain long enough to exceed LOG_DELTA_MAX, so one poll
        // provably cannot finish it.
        let mut text = String::new();
        let mut prev = "GENESIS".to_string();
        let mut lines = 0usize;
        while text.len() as u64 <= LOG_DELTA_MAX {
            let l = log_line(&prev, "r");
            prev = line_hash(&l);
            text.push_str(&l);
            text.push('\n');
            lines += 1;
        }
        std::fs::write(&path, &text).unwrap();

        let mut tail = LogTail::default();
        let first = log_snapshot_at(&path, &mut tail);
        assert!(
            first.total < lines as u64,
            "one poll consumed all {lines} lines, so the per-poll delta is not bounded"
        );
        assert!(
            tail.offset <= LOG_DELTA_MAX,
            "read past the cap in one poll"
        );
        assert!(
            first.chain_ok,
            "a bounded read must not look like a broken chain"
        );

        // Nothing was skipped: further polls converge on the whole file.
        let mut last = first;
        for _ in 0..8 {
            last = log_snapshot_at(&path, &mut tail);
            if last.total == lines as u64 {
                break;
            }
        }
        assert_eq!(
            last.total, lines as u64,
            "the capped remainder was never read"
        );
        assert!(last.chain_ok, "the chain broke across a poll boundary");
        assert_eq!(last.malformed, 0);
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn log_tail_drops_a_line_that_never_ends_instead_of_buffering_it() {
        let dir = std::env::temp_dir().join(format!("hooklog-line-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("hooks.log");

        // Bytes with no newline anywhere. The per-poll cap makes this
        // several polls; the point is that none of them keep the bytes.
        let huge = vec![b'{'; LOG_LINE_MAX * 3];
        std::fs::write(&path, &huge).unwrap();

        let mut tail = LogTail::default();
        let mut s = log_snapshot_at(&path, &mut tail);
        for _ in 0..8 {
            assert!(
                tail.partial.len() <= LOG_LINE_MAX,
                "an unterminated line is accumulating in memory: {} bytes buffered",
                tail.partial.len()
            );
            if tail.offset as usize >= huge.len() {
                break;
            }
            s = log_snapshot_at(&path, &mut tail);
        }
        assert_eq!(
            tail.offset as usize,
            huge.len(),
            "the file was never fully consumed"
        );
        assert!(
            tail.partial.is_empty(),
            "the refused bytes were kept after all"
        );
        assert_eq!(
            s.malformed, 1,
            "the dropped line must be counted exactly once"
        );
        assert!(
            !s.chain_ok,
            "a line that was never hashed cannot leave the chain verified"
        );

        // Resync: the newline that ends the refused line lets real events
        // be read again, so one hostile writer does not blind the panel
        // forever.
        let good = log_line("GENESIS", "after");
        std::fs::write(
            &path,
            format!("{}\n{}\n", String::from_utf8_lossy(&huge), good),
        )
        .unwrap();
        let mut s2 = log_snapshot_at(&path, &mut tail);
        for _ in 0..8 {
            if s2.events.len() == 1 {
                break;
            }
            s2 = log_snapshot_at(&path, &mut tail);
        }
        assert_eq!(
            s2.events.len(),
            1,
            "no event was parsed after the oversized line"
        );
        assert_eq!(s2.events[0].reason, "after");
        std::fs::remove_dir_all(&dir).ok();
    }
}
