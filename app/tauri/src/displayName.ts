// A session name on its way to a screen.
//
// The daemon refuses these names at creation (`at_valid_session_name`), so
// on a matched pair of binaries this function is a no-op. It exists because
// the GUI and the daemon ship and update separately: an agent-terminal
// v0.1.0 daemon — the released one — validates a name with the byte loop
// `*p == '/' || (unsigned char)*p < 0x20`, which accepts every codepoint
// this file treats, DEL included; its `ls` reply is what fills the sidebar.
// A GUI that renders a name it did not validate is trusting a version it
// cannot see.
//
// Two classes, two treatments, because the harm is not the same:
//
//   * Reordering characters (the twelve UAX #9 explicit formatting
//     characters — a closed set) are DELETED. They have no glyph of their
//     own; removing one leaves every visible glyph exactly as it was, in
//     the order the bytes say. `proj<U+202E>gol.hs` renders as the
//     `Kill proj.sdne ssecorp dlihc stI ?sh.log` that PR #81 measured, and
//     deleting the override is what makes the prompt name what it kills.
//
//   * Invisible characters are replaced by U+FFFD, NOT deleted. Deleting
//     them is what the attacker wants: `de<U+200B>ploy` would collapse to
//     exactly `deploy`, so the decoy row and the real row would render as
//     the same string — and clicking the wrong one sends keystrokes to
//     someone else's PTY. A visible mark keeps the two rows different,
//     which is the only property that matters here. Malformed UTF-8 already
//     arrives as U+FFFD from at-proto's `from_utf8_lossy`, so a suspicious
//     name already reads this way in the sidebar; this keeps one vocabulary.
//
// What this does NOT do: confusable letters. `dеploy` with a Cyrillic
// `е` (U+0435) is well-formed text and no character-class rule can judge
// it — SECURITY.md says so, and the pid in the kill confirmation is the
// mitigation that does not depend on reading the name.
//
// Names only. A notification BODY is arbitrary session output and is left
// exactly as the program wrote it; the reasoning is in SECURITY.md and the
// facts it rests on are measured in terminal/screenLine.test.ts.

/** What an invisible codepoint becomes. U+FFFD REPLACEMENT CHARACTER. */
export const INVISIBLE_MARK = "\uFFFD";

/** UAX #9 §2.6 explicit formatting characters — exactly the codepoints
 * that reorder their neighbours, and nothing else. */
function reorders(cp: number): boolean {
  return (
    cp === 0x061c || // ALM
    cp === 0x200e || // LRM
    cp === 0x200f || // RLM
    (cp >= 0x202a && cp <= 0x202e) || // LRE RLE PDF LRO RLO
    (cp >= 0x2066 && cp <= 0x2069) // LRI RLI FSI PDI
  );
}

/** Occupies no width of its own. Deliberately the same list as
 * `cp_misrepresents` in src/common/path.c minus the reordering set: the two
 * layers disagreeing about what counts as invisible would mean a name the
 * daemon accepts renders as a mark, or the reverse. Open-ended by nature —
 * Unicode adds codepoints — so this is a floor, not a proof. */
function invisible(cp: number): boolean {
  return (
    // C0 is the one class no released daemon serves. DEL and C1 both pass
    // v0.1.0's `< 0x20` byte test, so they are reachable, not ceremony.
    cp < 0x20 ||
    cp === 0x7f || // DEL
    (cp >= 0x80 && cp <= 0x9f) || // C1
    cp === 0x00ad || // SOFT HYPHEN
    cp === 0x034f || // COMBINING GRAPHEME JOINER
    cp === 0x180e || // MONGOLIAN VOWEL SEPARATOR
    (cp >= 0x200b && cp <= 0x200d) || // ZWSP ZWNJ ZWJ
    (cp >= 0x2028 && cp <= 0x2029) || // LINE / PARAGRAPH SEPARATOR
    (cp >= 0x2060 && cp <= 0x2064) || // WJ + invisible operators
    (cp >= 0xfe00 && cp <= 0xfe0f) || // variation selectors
    cp === 0xfeff || // ZWNBSP / BOM
    (cp >= 0xfff9 && cp <= 0xfffb) || // interlinear annotation
    (cp >= 0xe0000 && cp <= 0xe007f) // tag characters
  );
}

/** The form of a session name that is safe to render. Never use the result
 * to address a session: the daemon addresses sessions by their exact bytes,
 * so kill/attach/mute must carry the raw name. Mixing the two is how a
 * session becomes unkillable from its own sidebar row. */
export function displayName(raw: string): string {
  let out = "";
  for (const ch of raw) {
    const cp = ch.codePointAt(0)!;
    if (reorders(cp)) continue;
    out += invisible(cp) ? INVISIBLE_MARK : ch;
  }
  return out;
}
