import { describe, expect, it } from "vitest";
import {
  FIT_HINT_MIN_FRACTION,
  FIT_MIN_COLS,
  FIT_MIN_ROWS,
  FONT_DEFAULT,
  FONT_MAX,
  FONT_MIN,
  bottomScrollTop,
  fitGrid,
  isAtBottom,
  letterboxFraction,
  makeOneShotSet,
  nextFontSize,
  overflowsHost,
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

describe("fitGrid", () => {
  it("floors to whole cells — a rounded-up column wraps every line", () => {
    // 1000/8.4 = 119.04…, 700/17 = 41.2…
    expect(fitGrid(1000, 700, 8.4, 17)).toEqual({ cols: 119, rows: 41 });
  });
  it("clamps a tiny window to the usable floor", () => {
    expect(fitGrid(50, 30, 8, 17)).toEqual({ cols: FIT_MIN_COLS, rows: FIT_MIN_ROWS });
  });
  it("refuses unmeasurable input instead of sending a garbage grid", () => {
    expect(fitGrid(0, 700, 8, 17)).toBeNull(); // hidden host
    expect(fitGrid(1000, 700, 0, 17)).toBeNull(); // metrics not ready
  });
});

describe("letterboxFraction", () => {
  it("an 80×24 session in a big window is mostly letterbox (the complaint)", () => {
    // ~624×312 of terminal in a 1400×900 window ≈ 84% dead space.
    const f = letterboxFraction(1400, 900, 624, 312);
    expect(f).toBeGreaterThan(FIT_HINT_MIN_FRACTION);
    expect(f).toBeCloseTo(1 - (624 * 312) / (1400 * 900), 5);
  });
  it("a fitted session leaves only a sliver — below the hint threshold", () => {
    // One partial cell row/column of margin.
    expect(letterboxFraction(1400, 900, 1396, 892)).toBeLessThan(FIT_HINT_MIN_FRACTION);
  });
  it("unmeasurable input yields 0, not a garbage hint", () => {
    expect(letterboxFraction(0, 900, 624, 312)).toBe(0);
    expect(letterboxFraction(1400, 900, 0, 312)).toBe(0);
  });
  it("a terminal larger than the host never goes negative", () => {
    expect(letterboxFraction(800, 600, 1000, 700)).toBe(0);
  });
});

describe("overflowsHost", () => {
  // Measured pixel sizes from the browser probe with the app's own xterm
  // options at fontSize 13: an 80×24 grid renders 626×360 (cell 7.825×15).
  it("an 80×24 session fits an ordinary window", () => {
    expect(overflowsHost(900, 600, 626, 360)).toBe(false);
  });
  it("either axis alone is enough — one row too short still clips", () => {
    expect(overflowsHost(900, 359, 626, 360)).toBe(true);
    expect(overflowsHost(625, 600, 626, 360)).toBe(true);
  });
  it("exactly equal is not overflow (no scrollbar for a perfect fit)", () => {
    expect(overflowsHost(626, 360, 626, 360)).toBe(false);
  });
  it("unmeasurable input yields false, not a hint from garbage", () => {
    expect(overflowsHost(0, 600, 626, 360)).toBe(false);
    expect(overflowsHost(900, 600, 626, 0)).toBe(false);
  });
  it("a wide-but-short grid overflows AND leaves dead space", () => {
    // Both conditions hold at once, which is why the caller picks one
    // instead of treating them as complements: 1200 wide in a 900 window,
    // 120 tall in a 600 window.
    expect(overflowsHost(900, 600, 1200, 120)).toBe(true);
    expect(letterboxFraction(900, 600, 1200, 120)).toBeGreaterThan(
      FIT_HINT_MIN_FRACTION,
    );
  });
});

describe("bottomScrollTop", () => {
  it("scrolls a clipped grid to its last row", () => {
    expect(bottomScrollTop(200, 360)).toBe(160);
  });
  it("is zero when the grid fits — the letterbox stays below the text", () => {
    expect(bottomScrollTop(600, 360)).toBe(0);
    expect(bottomScrollTop(360, 360)).toBe(0);
  });
  it("unmeasurable input yields 0", () => {
    expect(bottomScrollTop(0, 360)).toBe(0);
    expect(bottomScrollTop(200, 0)).toBe(0);
  });
});

describe("makeOneShotSet", () => {
  it("consume answers true exactly once per add", () => {
    const s = makeOneShotSet();
    s.add("claude-1");
    expect(s.consume("claude-1")).toBe(true);
    // Re-selecting the session later must NOT re-impose geometry.
    expect(s.consume("claude-1")).toBe(false);
    expect(s.consume("never-added")).toBe(false);
  });
});

describe("windowTitle", () => {
  it("names the active session first", () => {
    expect(windowTitle("claude")).toBe("claude — agent-terminal");
  });
  it("falls back to the app name", () => {
    expect(windowTitle(null)).toBe("agent-terminal");
  });
  it("filters the name, because our app name sits downstream of it", () => {
    // Same shape as the notification title: one U+202E in a session name
    // reverses the " — agent-terminal" after it, so the window in
    // ⌘-Tab no longer identifies the app it belongs to.
    expect(windowTitle("proj\u202Egol.hs")).toBe("projgol.hs — agent-terminal");
    expect(windowTitle("de\u200Bploy")).toBe("de\uFFFDploy — agent-terminal");
  });
});
