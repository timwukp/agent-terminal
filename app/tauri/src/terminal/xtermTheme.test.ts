import { describe, expect, it } from "vitest";
import { DEFAULT_DARK } from "../theme";
import { SELECTION_ALPHA, xtermTheme } from "./xtermTheme";

describe("xtermTheme", () => {
  const t = xtermTheme(DEFAULT_DARK);

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

  it("does not touch the ANSI palette — session content owns its colors", () => {
    expect(t).not.toHaveProperty("red");
    expect(t).not.toHaveProperty("brightGreen");
    expect(t).not.toHaveProperty("black");
  });
});
