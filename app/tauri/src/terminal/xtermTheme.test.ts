import { describe, expect, it } from "vitest";
import { contrastRatio } from "../contrast";
import { DEFAULT_DARK, DEFAULT_LIGHT } from "../theme";
import { ANSI_LIGHT, SELECTION_ALPHA, xtermTheme } from "./xtermTheme";

describe("xtermTheme", () => {
  const t = xtermTheme(DEFAULT_DARK, "dark");

  it("cells sit on the letterbox color — the seam this file exists to kill", () => {
    expect(t.background).toBe(DEFAULT_DARK.bgMain);
  });

  it("text and cursor wear the text token; the cursor cell inverts", () => {
    expect(t.foreground).toBe(DEFAULT_DARK.text);
    expect(t.cursor).toBe(DEFAULT_DARK.text);
    expect(t.cursorAccent).toBe(DEFAULT_DARK.bgMain);
  });

  it("selection is the accent at alpha — solid would hide the selected glyphs", () => {
    expect(t.selectionBackground).toBe(DEFAULT_DARK.accent + SELECTION_ALPHA);
    expect(SELECTION_ALPHA).not.toBe("ff");
  });

  it("dark does not touch the ANSI palette — session content owns its colors", () => {
    expect(t).not.toHaveProperty("red");
    expect(t).not.toHaveProperty("brightGreen");
    expect(t).not.toHaveProperty("black");
  });
});

describe("the light ANSI palette", () => {
  const t = xtermTheme(DEFAULT_LIGHT, "light");

  it("light DOES set all 16 slots — xterm's defaults are dark-surface values", () => {
    // The gap this closes: xterm's own `white` (#d3d7cf) and `brightWhite`
    // (#eeeeec) are 1.46:1 and 1.16:1 on #ffffff, i.e. output the user
    // cannot read. Only 16 slots, and nothing else, comes from here.
    const slots = Object.keys(ANSI_LIGHT);
    expect(slots).toHaveLength(16);
    for (const slot of slots) expect(t).toHaveProperty(slot, ANSI_LIGHT[slot]);
  });

  it("keeps the surface tokens it was given", () => {
    expect(t.background).toBe(DEFAULT_LIGHT.bgMain);
    expect(t.foreground).toBe(DEFAULT_LIGHT.text);
  });

  it("every slot clears AA text contrast on the light terminal surface", () => {
    const failures = Object.entries(ANSI_LIGHT)
      .map(([slot, hex]) => ({
        slot,
        ratio: Number(contrastRatio(hex, DEFAULT_LIGHT.bgMain).toFixed(2)),
      }))
      .filter((r) => r.ratio < 4.5);
    expect(failures).toEqual([]);
  });

  it("no two slots are the same color", () => {
    // The white pair inverts (white → mid gray, brightWhite → darkest
    // ink), which is the one place a lazy palette collapses two slots
    // onto each other and loses a distinction the content was drawing.
    expect(new Set(Object.values(ANSI_LIGHT)).size).toBe(16);
  });

  it("the ratios the comment argues from are the measured ones", () => {
    // A number inside a justification comment is unchecked prose, and these
    // three ARE the argument for repainting the palette at all. Pinned as
    // data so a reworded comment cannot quietly keep a refuted figure.
    const on = (hex: string) => Number(contrastRatio(hex, DEFAULT_LIGHT.bgMain).toFixed(2));
    expect(on("#d3d7cf")).toBe(1.46); // xterm's own `white`
    expect(on("#eeeeec")).toBe(1.16); // xterm's own `brightWhite`
    expect(on("#34e2e2")).toBe(1.6); // xterm's own `brightCyan`
  });

  it("is not applied when the theme is dark", () => {
    // Mutation: drop the `name === "light"` guard and dark repaints every
    // CLI tool's output — the thing the dark comment promises not to do.
    expect(xtermTheme(DEFAULT_DARK, "dark")).not.toHaveProperty("blue");
  });
});
