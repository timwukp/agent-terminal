# hook-log — an opt-in, tamper-evident record of hook executions

Claude Code hooks run silently: settings.json names the scripts, but
nothing records what they decided, when, about what. The security card
(claude-panel.md) can therefore show *rules* but not *history* — unless
the user opts in to writing one. This document is that convention. The
GUI only ever **reads** it; nothing here is auto-installed.

## The file

`~/.claude/hooks/hooks.log` — append-only JSONL, one object per hook
execution:

```json
{"ts":"2026-08-11T12:00:00Z","hook":"block-git-push.sh","event":"PreToolUse","tool":"Bash","decision":"block","reason":"git push is PR-only here","prev":"GENESIS"}
{"ts":"2026-08-11T12:00:05Z","hook":"check-redaction.sh","event":"PreToolUse","tool":"Bash","decision":"allow","reason":"clean","prev":"9f8b1c2d…64 hex chars…"}
```

(Synthetic examples. Field values are free text; the GUI displays them
and colors `decision` — it attaches no semantics.)

`prev` is the lowercase-hex SHA-256 of the **previous line's raw bytes**
(without its trailing newline). The first line carries the literal
string `GENESIS`.

## What the chain is — and is not

This is a **plain hash chain: tamper-EVIDENT, not tamper-proof.**

On a single-user machine the hook scripts, this log, and any key you
could add all live under the same uid — and the supervised agent runs
as that uid too. A malicious same-uid process can rewrite the whole
file and recompute every hash in milliseconds. An HMAC whose key sits
on the same disk under the same uid changes that adversary's work
factor by approximately zero; it would be security theater with extra
key management. (A prior local experiment with exactly that shape —
same-disk key beside the log — is the cautionary example.)

What the chain **does** deliver:

1. **Accidental-damage detection** — truncation, a partial write from a
   crashed hook, a stray editor save: the break lands on a precise line.
2. **Ordering/interleaving integrity** — two hooks on one event (the
   common case) appending concurrently produce a detectable break
   rather than silently scrambled history. The wrapper below takes an
   exclusive lock for exactly this reason.
3. **Copy integrity** — a log moved between machines self-verifies.
4. A raised bar against **casual** edits — not against an adversary.

**Upgrade path (out of scope here):** real tamper-resistance requires
the verification key to live outside the uid's write reach — macOS
Keychain with an ACL, or a remote append-only sink. If that lands, it
lands as a new field beside `prev`, not a format break.

## The wrapper

Add to the END of a hook script (after it has decided), or wrap the
script with it. Requires only python3; `flock` makes concurrent hooks
serialize instead of corrupting the chain:

```sh
# hook-log: append a tamper-evident record (see app/design/hook-log.md)
log_hook_event() { # $1=hook $2=event $3=tool $4=decision $5=reason
  python3 - "$1" "$2" "$3" "$4" "$5" <<'PY'
import fcntl, hashlib, json, os, sys, datetime
path = os.path.expanduser("~/.claude/hooks/hooks.log")
os.makedirs(os.path.dirname(path), exist_ok=True)
with open(path, "a+b") as f:
    fcntl.flock(f, fcntl.LOCK_EX)
    f.seek(0)
    data = f.read()
    lines = data.splitlines()
    prev = hashlib.sha256(lines[-1]).hexdigest() if lines else "GENESIS"
    hook, event, tool, decision, reason = sys.argv[1:6]
    rec = {"ts": datetime.datetime.now(datetime.timezone.utc)
              .strftime("%Y-%m-%dT%H:%M:%SZ"),
           "hook": hook, "event": event, "tool": tool,
           "decision": decision, "reason": reason, "prev": prev}
    f.write((json.dumps(rec, ensure_ascii=False) + "\n").encode())
PY
}
```

## What the GUI does with it

The security card (in the Hooks tab) tails the file when present,
verifies the chain incrementally (only appended bytes are re-hashed),
and shows the most recent events with a badge:

- `chain verified · N events`
- `chain broken at line K` — everything after the break still displays;
  a break is a fact about the file, not a reason to hide history.
- No file → the card says plainly that hook executions are not being
  recorded, and points here.

Both reads are bounded, because the file is append-only and nothing
constrains who appends or how fast:

- **At most 1 MiB per poll.** Nothing is skipped — the cursor advances
  only by what was consumed, so a backlog is verified across the next few
  polls instead of inside one blocking call. `N events` therefore counts
  what has been *verified*, which is the honest number: an unread line has
  no verdict yet.
- **At most 1 MiB in one unterminated line.** The per-poll cap bounds work
  but not memory: bytes with no newline in them are held for the next poll
  and accumulate for as long as a writer withholds the newline. A hook
  event is one JSON object on one line, so past that it is not one — it is
  counted malformed and its bytes are dropped rather than buffered. A
  dropped line cannot be hashed, so the chain verdict stays broken, which
  is the accurate report; the next newline resynchronizes, so one bad
  writer does not blind the card forever.
