import { describe, expect, it } from "vitest";
import {
  FONT_DEFAULT,
  FONT_MAX,
  FONT_MIN,
  isAtBottom,
  nextFontSize,
  windowTitle,
  zoomActionForKey,
} from "./viewControls";

describe("zoomActionForKey", () => {
  const cases: Array<[string, { key: string; metaKey: boolean; ctrlKey: boolean }, string | null]> =
    [
      ["cmd-plus zooms in", { key: "+", metaKey: true, ctrlKey: false }, "in"],
      // US keyboards send '=' for the unshifted key under the '+' cap;
      // requiring shift would make zoom-in appear broken on exactly the
      // most common layout.
      ["cmd-equals zooms in", { key: "=", metaKey: true, ctrlKey: false }, "in"],
      ["ctrl-minus zooms out", { key: "-", metaKey: false, ctrlKey: true }, "out"],
      ["cmd-zero resets", { key: "0", metaKey: true, ctrlKey: false }, "reset"],
      ["bare '=' goes to the shell", { key: "=", metaKey: false, ctrlKey: false }, null],
      ["bare '0' goes to the shell", { key: "0", metaKey: false, ctrlKey: false }, null],
      ["cmd-c is not zoom (copy must survive)", { key: "c", metaKey: true, ctrlKey: false }, null],
    ];
  it.each(cases)("%s", (_name, ev, want) => {
    expect(zoomActionForKey(ev)).toBe(want);
  });
});

describe("nextFontSize", () => {
  it("steps by one point", () => {
    expect(nextFontSize(13, "in")).toBe(14);
    expect(nextFontSize(13, "out")).toBe(12);
  });
  it("clamps at both ends instead of walking past them", () => {
    expect(nextFontSize(FONT_MAX, "in")).toBe(FONT_MAX);
    expect(nextFontSize(FONT_MIN, "out")).toBe(FONT_MIN);
  });
  it("reset returns the constructor default from anywhere", () => {
    expect(nextFontSize(FONT_MAX, "reset")).toBe(FONT_DEFAULT);
    expect(nextFontSize(FONT_MIN, "reset")).toBe(FONT_DEFAULT);
  });
});

describe("isAtBottom", () => {
  it("equal viewport and base = live bottom", () => {
    expect(isAtBottom(120, 120)).toBe(true);
  });
  it("viewport above base = scrolled up (pill shows)", () => {
    expect(isAtBottom(80, 120)).toBe(false);
  });
  it("empty buffer (both zero) is at bottom, not 'behind'", () => {
    expect(isAtBottom(0, 0)).toBe(true);
  });
});

describe("windowTitle", () => {
  it("names the active session first", () => {
    expect(windowTitle("claude")).toBe("claude — agent-terminal");
  });
  it("falls back to the app name", () => {
    expect(windowTitle(null)).toBe("agent-terminal");
  });
});
