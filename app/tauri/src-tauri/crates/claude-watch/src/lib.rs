//! Claude Code transcript watcher (design: app/design/claude-panel.md).
//!
//! Reads `~/.claude/projects/<cwd-slug>/<session>.jsonl` and turns the
//! `usage` objects on assistant lines into per-transcript token totals.
//! The schema is NOT a public contract, so everything here is lenient:
//! unknown fields ignored, usage-less lines skipped, malformed lines
//! *counted* (the panel shows "N unparsed") but never an error.
//!
//! Layering mirrors at-proto: a pure core (`parse_line`, `Accumulator`,
//! `Cursor`) that owns every decision and is table-tested, plus a thin
//! filesystem shell (`Watcher`) that only stats, seeks and reads.

use serde::Serialize;
use std::collections::HashMap;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

/// Map a working directory to Claude Code's project slug: every `/`
/// becomes `-`. `/opt/proj` → `-opt-proj`.
pub fn project_slug(cwd: &str) -> String {
    cwd.replace('/', "-")
}

/// One assistant message's usage, as observed in real transcripts
/// (2026-08: input/output plus the two cache counters; several sibling
/// fields exist and are deliberately not modeled).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize)]
pub struct Usage {
    pub input_tokens: u64,
    pub output_tokens: u64,
    pub cache_read_input_tokens: u64,
    pub cache_creation_input_tokens: u64,
}

/// The parts of one transcript line the panel cares about.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UsageEvent {
    /// `message.id`. Claude Code writes one line per content block, so
    /// one API message appears up to N times WITH IDENTICAL USAGE —
    /// measured 6× in a real transcript. Summing without deduplicating
    /// by this id overcounts by that factor.
    pub message_id: String,
    pub model: String,
    /// ISO-8601 UTC (`2026-08-11T13:42:57.292Z`). Kept as text: the
    /// format sorts lexicographically and the minute bucket is a prefix.
    pub timestamp: String,
    pub usage: Usage,
}

/// What one line meant. Most lines are `Irrelevant` by design (user
/// turns, attachments, file-history snapshots); `Malformed` feeds the
/// "N unparsed" badge instead of an error.
pub enum ParseOutcome {
    Event(UsageEvent),
    /// Valid JSON, just not an assistant-usage line. Not an anomaly.
    Irrelevant,
    /// Not JSON, or an assistant line missing what the panel needs.
    Malformed,
}

pub fn parse_line(line: &str) -> ParseOutcome {
    if line.trim().is_empty() {
        return ParseOutcome::Irrelevant;
    }
    let Ok(v) = serde_json::from_str::<serde_json::Value>(line) else {
        return ParseOutcome::Malformed;
    };
    if v.get("type").and_then(|t| t.as_str()) != Some("assistant") {
        return ParseOutcome::Irrelevant;
    }
    let Some(msg) = v.get("message") else {
        return ParseOutcome::Malformed;
    };
    let Some(u) = msg.get("usage") else {
        // Assistant line without usage: seen for synthetic/error
        // placeholders; nothing to count, nothing wrong.
        return ParseOutcome::Irrelevant;
    };
    let field = |k: &str| u.get(k).and_then(|x| x.as_u64()).unwrap_or(0);
    let Some(message_id) = msg.get("id").and_then(|x| x.as_str()) else {
        // Usage that cannot be deduplicated would overcount ~6×;
        // dropping it under-counts once. Under-counting is the honest
        // failure, and the badge says it happened.
        return ParseOutcome::Malformed;
    };
    ParseOutcome::Event(UsageEvent {
        message_id: message_id.to_string(),
        model: msg
            .get("model")
            .and_then(|x| x.as_str())
            .unwrap_or("<unknown>")
            .to_string(),
        timestamp: v
            .get("timestamp")
            .and_then(|x| x.as_str())
            .unwrap_or("")
            .to_string(),
        usage: Usage {
            input_tokens: field("input_tokens"),
            output_tokens: field("output_tokens"),
            cache_read_input_tokens: field("cache_read_input_tokens"),
            cache_creation_input_tokens: field("cache_creation_input_tokens"),
        },
    })
}

/// Per-minute output-token bucket (sparkline food). `minute` is the
/// 16-char ISO prefix `YYYY-MM-DDTHH:MM`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Bucket {
    pub minute: String,
    pub output_tokens: u64,
}

/// Keep this many most-recent minute buckets per transcript. Half an
/// hour fills the sparkline; older activity is already in the totals.
pub const BUCKETS_MAX: usize = 30;

/// Accumulates one transcript's events into panel-ready numbers.
#[derive(Debug, Default, Serialize)]
pub struct Accumulator {
    pub totals: Usage,
    /// Unique API messages (after dedup), not lines.
    pub messages: u64,
    pub malformed: u64,
    /// Model seen on the newest event — sessions do switch models.
    pub model: String,
    /// Timestamp of the newest event (ISO text, lexicographic max).
    pub last_timestamp: String,
    pub buckets: Vec<Bucket>,
    /// message.id → (usage already counted, the timestamp it was counted
    /// under). Repeats with identical usage are the norm (one line per
    /// content block); a repeat with DIFFERENT usage replaces its
    /// predecessor in the totals rather than double-counting.
    ///
    /// The timestamp is kept because the retraction has to hit the minute
    /// bucket the tokens went INTO, and a finalized usage can arrive in a
    /// later minute than the one it was first counted in.
    #[serde(skip)]
    seen: HashMap<String, (Usage, String)>,
}

fn add(t: &mut Usage, u: &Usage) {
    t.input_tokens += u.input_tokens;
    t.output_tokens += u.output_tokens;
    t.cache_read_input_tokens += u.cache_read_input_tokens;
    t.cache_creation_input_tokens += u.cache_creation_input_tokens;
}

fn sub(t: &mut Usage, u: &Usage) {
    t.input_tokens -= u.input_tokens;
    t.output_tokens -= u.output_tokens;
    t.cache_read_input_tokens -= u.cache_read_input_tokens;
    t.cache_creation_input_tokens -= u.cache_creation_input_tokens;
}

impl Accumulator {
    pub fn feed(&mut self, line: &str) {
        match parse_line(line) {
            ParseOutcome::Irrelevant => {}
            ParseOutcome::Malformed => self.malformed += 1,
            ParseOutcome::Event(ev) => self.apply(ev),
        }
    }

    fn apply(&mut self, ev: UsageEvent) {
        match self.seen.get(&ev.message_id) {
            Some((prev, _)) if *prev == ev.usage => return, // routine repeat
            Some((prev, prev_ts)) => {
                // The message's usage was rewritten: keep the newest. The
                // bucket credited earlier is the one identified by the
                // PREVIOUS timestamp — `ev.timestamp` may be a later
                // minute, and subtracting there would strand the old
                // tokens in their bucket and take tokens out of a bucket
                // that never held them.
                let prev = *prev;
                let prev_ts = prev_ts.clone();
                sub(&mut self.totals, &prev);
                self.bucket_sub(&prev_ts, prev.output_tokens);
                self.messages -= 1;
            }
            None => {}
        }
        add(&mut self.totals, &ev.usage);
        self.messages += 1;
        self.bucket_add(&ev.timestamp, ev.usage.output_tokens);
        if ev.timestamp >= self.last_timestamp {
            self.last_timestamp = ev.timestamp.clone();
            self.model = ev.model.clone();
        }
        self.seen.insert(ev.message_id, (ev.usage, ev.timestamp));
    }

    fn bucket_add(&mut self, ts: &str, tokens: u64) {
        if ts.len() < 16 {
            return;
        }
        let minute = &ts[..16];
        match self.buckets.iter_mut().find(|b| b.minute == minute) {
            Some(b) => b.output_tokens += tokens,
            None => {
                self.buckets.push(Bucket {
                    minute: minute.to_string(),
                    output_tokens: tokens,
                });
                // Appends arrive in time order so this stays sorted;
                // sort anyway — a clock step backwards must not
                // silently disorder the sparkline.
                self.buckets.sort_by(|a, b| a.minute.cmp(&b.minute));
                if self.buckets.len() > BUCKETS_MAX {
                    let drop = self.buckets.len() - BUCKETS_MAX;
                    self.buckets.drain(..drop);
                }
            }
        }
    }

    fn bucket_sub(&mut self, ts: &str, tokens: u64) {
        if ts.len() < 16 {
            return;
        }
        if let Some(b) = self.buckets.iter_mut().find(|b| b.minute == ts[..16]) {
            b.output_tokens = b.output_tokens.saturating_sub(tokens);
        }
    }
}

/// Byte-offset cursor over one growing JSONL file. Owns exactly two
/// decisions: resume where the last read ended, and never feed a
/// partial line to the parser (a line split across reads would count
/// as malformed and then be recounted whole).
#[derive(Debug, Default)]
pub struct Cursor {
    offset: u64,
    partial: Vec<u8>,
}

impl Cursor {
    /// Digest `chunk` (the bytes at `self.offset()`) into `acc`.
    pub fn feed_chunk(&mut self, chunk: &[u8], acc: &mut Accumulator) {
        self.offset += chunk.len() as u64;
        self.partial.extend_from_slice(chunk);
        // Split on \n, keep the trailing partial line for the next read.
        while let Some(pos) = self.partial.iter().position(|&b| b == b'\n') {
            let line: Vec<u8> = self.partial.drain(..=pos).collect();
            acc.feed(&String::from_utf8_lossy(&line[..line.len() - 1]));
        }
    }

    /// Where the next read should start. If the file shrank (rotation,
    /// truncation) the caller resets and re-reads from zero.
    pub fn offset(&self) -> u64 {
        self.offset
    }
}

/// Snapshot of one transcript, panel-ready.
#[derive(Debug, Serialize)]
pub struct TranscriptUsage {
    /// File stem — Claude Code names transcripts `<session-uuid>.jsonl`.
    pub id: String,
    /// Project slug directory name (the cwd, dashes for slashes).
    pub project: String,
    pub totals: Usage,
    pub messages: u64,
    pub malformed: u64,
    pub model: String,
    pub last_timestamp: String,
    pub buckets: Vec<Bucket>,
}

/// Only transcripts written to within this window are tailed. Keeps the
/// per-poll stat set proportional to *live* work, not to history.
pub const ACTIVE_WINDOW_SECS: u64 = 48 * 3600;

struct Tail {
    cursor: Cursor,
    acc: Accumulator,
}

/// Filesystem shell: stat + read the active transcripts under a
/// projects root. All parsing decisions live in the pure core above.
pub struct Watcher {
    root: PathBuf,
    tails: HashMap<PathBuf, Tail>,
}

impl Watcher {
    /// `root` is `~/.claude/projects` in production, a temp dir in tests.
    pub fn new(root: PathBuf) -> Self {
        Self {
            root,
            tails: HashMap::new(),
        }
    }

    /// Poll every active transcript and return current numbers, newest
    /// activity first. Cheap when nothing changed: one stat per file,
    /// reads only appended bytes.
    pub fn snapshot(&mut self) -> Vec<TranscriptUsage> {
        for path in list_active(&self.root) {
            let tail = self.tails.entry(path.clone()).or_insert_with(|| Tail {
                cursor: Cursor::default(),
                acc: Accumulator::default(),
            });
            let Ok(meta) = std::fs::metadata(&path) else {
                continue;
            };
            if meta.len() < tail.cursor.offset() {
                // Truncated or rewritten: what was counted no longer
                // exists. Start the file over.
                *tail = Tail {
                    cursor: Cursor::default(),
                    acc: Accumulator::default(),
                };
            }
            if meta.len() > tail.cursor.offset() {
                if let Ok(chunk) = read_from(&path, tail.cursor.offset()) {
                    tail.cursor.feed_chunk(&chunk, &mut tail.acc);
                }
            }
        }
        let mut out: Vec<TranscriptUsage> = self
            .tails
            .iter()
            .filter(|(_, t)| t.acc.messages > 0 || t.acc.malformed > 0)
            .map(|(path, t)| TranscriptUsage {
                id: path
                    .file_stem()
                    .map(|s| s.to_string_lossy().into_owned())
                    .unwrap_or_default(),
                project: path
                    .parent()
                    .and_then(|p| p.file_name())
                    .map(|s| s.to_string_lossy().into_owned())
                    .unwrap_or_default(),
                totals: t.acc.totals,
                messages: t.acc.messages,
                malformed: t.acc.malformed,
                model: t.acc.model.clone(),
                last_timestamp: t.acc.last_timestamp.clone(),
                buckets: t.acc.buckets.clone(),
            })
            .collect();
        out.sort_by(|a, b| b.last_timestamp.cmp(&a.last_timestamp));
        out
    }
}

fn list_active(root: &Path) -> Vec<PathBuf> {
    let cutoff = std::time::SystemTime::now() - std::time::Duration::from_secs(ACTIVE_WINDOW_SECS);
    let mut out = Vec::new();
    let Ok(projects) = std::fs::read_dir(root) else {
        return out;
    };
    for project in projects.flatten() {
        let Ok(files) = std::fs::read_dir(project.path()) else {
            continue;
        };
        for f in files.flatten() {
            let p = f.path();
            if p.extension().and_then(|e| e.to_str()) != Some("jsonl") {
                continue;
            }
            let fresh = f
                .metadata()
                .and_then(|m| m.modified())
                .map(|m| m >= cutoff)
                .unwrap_or(false);
            if fresh {
                out.push(p);
            }
        }
    }
    out
}

fn read_from(path: &Path, offset: u64) -> std::io::Result<Vec<u8>> {
    let mut f = std::fs::File::open(path)?;
    f.seek(SeekFrom::Start(offset))?;
    let mut buf = Vec::new();
    f.read_to_end(&mut buf)?;
    Ok(buf)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn slug_replaces_every_slash() {
        assert_eq!(project_slug("/opt/proj"), "-opt-proj");
    }

    #[test]
    fn root_is_single_dash() {
        assert_eq!(project_slug("/"), "-");
    }
}
