import { describe, expect, it } from "vitest";
import { paneRectToPixels, readCellMetrics } from "./overlay";
import { paneAtPixel } from "./hittest";
import type { PaneRect } from "./transport";

const M = { cellWidth: 8.5, cellHeight: 17.25 }; // deliberately fractional

const LEFT: PaneRect = { id: 1, x: 0, y: 0, cols: 40, rows: 24 };
const RIGHT: PaneRect = { id: 2, x: 41, y: 0, cols: 39, rows: 24 };

describe("paneRectToPixels", () => {
  it("scales cell origin and extent by the cell size", () => {
    expect(paneRectToPixels(RIGHT, M)).toEqual({
      left: 41 * 8.5,
      top: 0,
      width: 39 * 8.5,
      height: 24 * 17.25,
    });
  });

  it("a pane at a non-zero row offset is not pinned to the top", () => {
    // The stacked-split case. An overlay that ignored y would sit on the
    // wrong pane while the hit-test still worked, so the two must agree.
    const bottom: PaneRect = { id: 2, x: 0, y: 13, cols: 80, rows: 11 };
    const r = paneRectToPixels(bottom, M);
    expect(r.top).toBeCloseTo(13 * 17.25);
    expect(r.height).toBeCloseTo(11 * 17.25);
  });

  it("round-trips: every corner inside the drawn box hits that pane", () => {
    // This is the actual guarantee worth having — the overlay the user
    // sees and the region that responds to a click are the same region.
    // A cell-size mismatch between the two breaks this test rather than
    // shipping an overlay that lies about where the pane is.
    for (const pane of [LEFT, RIGHT]) {
      const r = paneRectToPixels(pane, M);
      const eps = 0.01;
      const corners: [number, number][] = [
        [r.left + eps, r.top + eps],
        [r.left + r.width - eps, r.top + eps],
        [r.left + eps, r.top + r.height - eps],
        [r.left + r.width - eps, r.top + r.height - eps],
        [r.left + r.width / 2, r.top + r.height / 2],
      ];
      for (const [px, py] of corners) {
        expect(paneAtPixel(px, py, M, [LEFT, RIGHT])).toBe(pane.id);
      }
    }
  });

  it("the gap between two drawn boxes is the divider column", () => {
    // Pane 1 ends at col 40, pane 2 starts at col 41: column 40 belongs to
    // neither, and clicking it must not select a pane (hittest.ts).
    const l = paneRectToPixels(LEFT, M);
    const r = paneRectToPixels(RIGHT, M);
    expect(r.left).toBeGreaterThan(l.left + l.width);
    const mid = (l.left + l.width + r.left) / 2;
    expect(paneAtPixel(mid, 10, M, [LEFT, RIGHT])).toBeNull();
  });
});

describe("readCellMetrics", () => {
  it("reads xterm's private render dimensions", () => {
    // xterm names these `width`/`height`; our CellMetrics names them
    // `cellWidth`/`cellHeight`. The rename is the whole point of this
    // function, so the fixture must use xterm's spelling.
    const term = {
      _core: { _renderService: { dimensions: { css: { cell: { width: 8.5, height: 17.25 } } } } },
    };
    expect(readCellMetrics(term)).toEqual(M);
  });

  it("returns null instead of throwing when the private shape is absent", () => {
    // A xterm major bump must degrade to "no overlay", not a crash: this
    // reads private API, so it is the one thing guaranteed to change.
    expect(readCellMetrics({})).toBeNull();
    expect(readCellMetrics(null)).toBeNull();
    expect(readCellMetrics(undefined)).toBeNull();
    expect(readCellMetrics({ _core: {} })).toBeNull();
    expect(readCellMetrics({ _core: { _renderService: { dimensions: {} } } })).toBeNull();
  });

  it("rejects degenerate sizes that would place every overlay at 0×0", () => {
    // Before the first render the cell can measure 0, and 0 would divide
    // to Infinity in the hit-test. NaN is rejected by the same > 0 check.
    for (const cell of [
      { width: 0, height: 17 },
      { width: 8, height: 0 },
      { width: NaN, height: 17 },
      { width: -8, height: 17 },
    ]) {
      const term = { _core: { _renderService: { dimensions: { css: { cell } } } } };
      expect(readCellMetrics(term)).toBeNull();
    }
  });
});
