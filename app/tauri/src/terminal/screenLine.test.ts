// @vitest-environment jsdom
//
// What a hostile program can put in an OS notification body, measured with
// the real xterm parser rather than reasoned about. This file is the
// evidence for a DECISION recorded in SECURITY.md: the notification body is
// passed through untouched while a session NAME is filtered for display
// (../displayName.ts). That decision is only defensible while these
// measurements hold, so they are assertions — if a future xterm starts
// passing C1 through, or stops passing U+202E, the decision reopens and this
// file is what says so.
//
// Payloads are built from numeric codepoints, never typed literally: a
// source file carrying a raw RIGHT-TO-LEFT OVERRIDE renders its own text
// reversed, which is the attack being tested (CVE-2021-42574).
//
// xterm is real here, and deliberately never `open()`ed — the parser and
// buffer need no DOM, and jsdom has no canvas.

import { describe, expect, it } from "vitest";
import { Terminal } from "@xterm/xterm";
import { lastNonEmptyLine } from "./screenLine";

const COLS = 80;

/** Write `payload` to a fresh terminal and read back what a notification
 * body would carry. The callback form of write() is the only way to know the
 * parser has drained; xterm writes are asynchronous. */
async function bodyFor(payload: string, cols = COLS): Promise<string> {
  const term = new Terminal({ cols, rows: 24, allowProposedApi: true });
  try {
    await new Promise<void>((resolve) => term.write(payload, resolve));
    return lastNonEmptyLine(term);
  } finally {
    term.dispose();
  }
}

/** "a<cp>b" — a codepoint with a visible neighbour on each side, so a
 * survivor is unambiguous and a dropped one leaves "ab". */
const sandwich = (cp: number) => "a" + String.fromCodePoint(cp) + "b";

// UAX #9 §2.6, the closed set of explicit formatting characters.
const REORDERING: Array<[string, number]> = [
  ["ALM", 0x061c],
  ["LRM", 0x200e],
  ["RLM", 0x200f],
  ["LRE", 0x202a],
  ["RLE", 0x202b],
  ["PDF", 0x202c],
  ["LRO", 0x202d],
  ["RLO", 0x202e],
  ["LRI", 0x2066],
  ["RLI", 0x2067],
  ["FSI", 0x2068],
  ["PDI", 0x2069],
];

// Consumed by the parser — ignored, obeyed, or opening a sequence — so none
// of them can appear in a cell. This is why the body needs no control
// filter: the class cannot arrive. The exact output is asserted rather than
// "does not contain", because a body that came back empty would satisfy
// "does not contain" for every one of them.
//
// C1 is not merely dropped: CSI (U+009B) followed by `b` is REP, which
// repeats the preceding character — "a<CSI>b" yields "aa". Acted upon, and
// still unable to place the introducer itself in the body.
const CONTROLS: Array<[string, number, string]> = [
  ["NUL", 0x0000, "ab"], // ignored
  ["BEL", 0x0007, "ab"], // rings; prints nothing
  ["BS", 0x0008, "b"], // cursor back one, so `b` overwrites `a`
  ["LF", 0x000a, "b"], // next row, and the newest row wins
  ["CR", 0x000d, "b"], // column 0, so `b` overwrites `a`
  ["ESC", 0x001b, "a"], // ESC b is a sequence; the `b` is eaten with it
  ["DEL", 0x007f, "ab"], // ignored
  ["NEL (C1)", 0x0085, "b"], // C1 newline
  ["CSI (C1)", 0x009b, "aa"], // CSI b = REP, repeat the previous character
];

describe("what reaches a notification body", () => {
  it.each(REORDERING)("passes %s straight through", async (_label, cp) => {
    expect(await bodyFor(sandwich(cp))).toBe(sandwich(cp));
  });

  it.each(CONTROLS)("never yields %s", async (_label, cp, expected) => {
    const body = await bodyFor(sandwich(cp));
    expect(body).toBe(expected);
    expect(body).not.toContain(String.fromCodePoint(cp));
  });

  it("passes zero-width characters through, except the BOM", async () => {
    // xterm drops U+FEFF on its own; the rest are stored in cells. Not our
    // doing either way — asserted so the list is a measurement, not a
    // guess, since displayName.ts treats all of them alike for NAMES.
    for (const cp of [0x200b, 0x200c, 0x200d, 0x2060, 0x00ad, 0x034f, 0xfe0f]) {
      expect(await bodyFor(sandwich(cp))).toBe(sandwich(cp));
    }
    expect(await bodyFor(sandwich(0xfeff))).toBe("ab");
  });

  it("passes U+2028 and U+2029, the only survivors that can look like a line break", async () => {
    for (const cp of [0x2028, 0x2029]) {
      expect(await bodyFor(sandwich(cp))).toBe(sandwich(cp));
    }
  });

  it("cannot exceed the session's width, however much is written", async () => {
    // The body is one row of a fixed grid, and the daemon clamps cols to
    // VT_COLS_MAX at the wire edge (S2), so its length is bounded without a
    // cap here. 400 columns of text wrap; the last row holds the remainder.
    expect((await bodyFor("x".repeat(400))).length).toBe(COLS);
    expect((await bodyFor("y".repeat(200))).length).toBe(200 - 2 * COLS);
    expect((await bodyFor("z".repeat(10), 20)).length).toBe(10);
  });

  it("strips SGR colour and reads the newest row", async () => {
    expect(await bodyFor("\u001b[31mred\u001b[0m tail")).toBe("red tail");
    expect(await bodyFor("first\r\nsecond")).toBe("second");
  });

  it("keeps text a program legitimately prints", async () => {
    // The cost side of the decision not to filter bodies: emoji built from
    // ZWJ sequences and variation selectors are ordinary CLI output, and a
    // filter strict enough to catch invisible characters would break them.
    const dev = "\u{1F469}\u200D\u{1F4BB} ok";
    expect(await bodyFor(dev)).toBe(dev);
    expect(await bodyFor("build ✓ 2 files")).toBe("build ✓ 2 files");
  });

  it("is empty for a terminal nothing has written to", async () => {
    expect(await bodyFor("")).toBe("");
    expect(await bodyFor("   \r\n   ")).toBe("");
  });
});
