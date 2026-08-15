// displayName: the two treatments and the line between them.
//
// Payloads are built from numeric codepoints, never typed literally — a file
// carrying a raw RIGHT-TO-LEFT OVERRIDE renders its own source reversed,
// which is the thing under test.

import { describe, expect, it } from "vitest";
import { INVISIBLE_MARK, displayName } from "./displayName";

const ch = (cp: number) => String.fromCodePoint(cp);

// UAX #9 §2.6 — the closed set. Every one of these is deleted.
const REORDERING = [
  0x061c, 0x200e, 0x200f, 0x202a, 0x202b, 0x202c, 0x202d, 0x202e, 0x2066, 0x2067, 0x2068, 0x2069,
];

// Occupies no width. Every one of these becomes a visible mark.
const INVISIBLE = [
  0x0000, 0x001f, 0x007f, 0x0085, 0x009b, 0x00ad, 0x034f, 0x180e, 0x200b, 0x200c, 0x200d, 0x2028,
  0x2029, 0x2060, 0x2064, 0xfe00, 0xfe0f, 0xfeff, 0xfff9, 0xfffb, 0xe0001, 0xe007f,
];

describe("displayName", () => {
  it("deletes every reordering character, leaving the glyphs untouched", () => {
    for (const cp of REORDERING) {
      expect(displayName("proj" + ch(cp) + "gol.hs")).toBe("projgol.hs");
    }
  });

  it("marks every invisible character instead of deleting it", () => {
    for (const cp of INVISIBLE) {
      expect(displayName("de" + ch(cp) + "ploy")).toBe("de" + INVISIBLE_MARK + "ploy");
    }
  });

  it("keeps a decoy distinguishable from the name it impersonates", () => {
    // The reason invisibles are marked and not stripped. Deleting U+200B
    // would make these two strings equal, and then two sidebar rows read the
    // same while only one of them is the session you meant.
    const real = "deploy";
    const decoy = "de" + ch(0x200b) + "ploy";
    expect(decoy).not.toBe(real);
    expect(displayName(decoy)).not.toBe(displayName(real));
  });

  it("leaves an ordinary name exactly as it is", () => {
    // Both Unicode forms of the same word: "caf\u00E9" precomposed, and
    // "e\u0301clair" with a combining acute, which is a Mn character this
    // must not touch — invisible is not the same thing as zero-width.
    const names = ["work", "agent-3", "a_b.c", "проект", "作業", "café", "e\u0301clair"];
    for (const name of names) {
      expect(displayName(name)).toBe(name);
    }
  });

  it("does not judge confusable letters — the documented gap", () => {
    // Cyrillic small letter IE (U+0435) in place of the Latin one. Both
    // survive intact and render identically; nothing here can tell them
    // apart, which is why the kill confirmation shows the pid.
    const latin = "deploy";
    const cyrillic = "d" + ch(0x0435) + "ploy";
    expect(displayName(cyrillic)).toBe(cyrillic);
    expect(displayName(cyrillic)).not.toBe(displayName(latin));
  });

  it("passes through a U+FFFD that arrived on its own", () => {
    // at-proto decodes names with from_utf8_lossy, so a malformed name is
    // already U+FFFD before it gets here. One vocabulary, not two.
    expect(displayName("pro\uFFFDj")).toBe("pro\uFFFDj");
  });

  it("handles the empty string and a name that is nothing but payload", () => {
    expect(displayName("")).toBe("");
    expect(displayName(ch(0x202e))).toBe("");
    expect(displayName(ch(0x200b) + ch(0x200b))).toBe(INVISIBLE_MARK + INVISIBLE_MARK);
  });

  it("counts by codepoint, not by UTF-16 unit", () => {
    // Tag characters live above the BMP: iterating with `for..of` sees one
    // codepoint, while a charCodeAt loop would see two surrogates and
    // recognise neither. An emoji must survive that same loop unharmed.
    expect(displayName("a" + ch(0xe0041) + "b")).toBe("a" + INVISIBLE_MARK + "b");
    expect(displayName("a\u{1F600}b")).toBe("a\u{1F600}b");
  });

  it("is idempotent", () => {
    const nasty = "p" + ch(0x202e) + "r" + ch(0x200b) + "oj";
    expect(displayName(displayName(nasty))).toBe(displayName(nasty));
  });
});
