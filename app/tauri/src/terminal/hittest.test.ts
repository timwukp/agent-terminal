import { describe, expect, it } from "vitest";
import { paneAtCell, paneAtPixel, pixelToCell } from "./hittest";
import type { PaneRect } from "./transport";

// The discriminating topology from the C repo's directional tests: a
// full-height left pane and two stacked right panes, 120x40 view.
// Divider columns/rows (x=60, right y=20) belong to no pane.
const PANES: PaneRect[] = [
  { id: 0, x: 0, y: 0, cols: 60, rows: 40 },
  { id: 1, x: 61, y: 0, cols: 59, rows: 20 },
  { id: 2, x: 61, y: 21, cols: 59, rows: 19 },
];

describe("pixelToCell", () => {
  const m = { cellWidth: 8, cellHeight: 16 };
  it("floors into the containing cell", () => {
    expect(pixelToCell(0, 0, m)).toEqual({ col: 0, row: 0 });
    expect(pixelToCell(7.9, 15.9, m)).toEqual({ col: 0, row: 0 });
    expect(pixelToCell(8, 16, m)).toEqual({ col: 1, row: 1 });
    expect(pixelToCell(487.5, 639.5, m)).toEqual({ col: 60, row: 39 });
  });
  it("handles fractional cell metrics (real font measurements)", () => {
    const frac = { cellWidth: 7.8125, cellHeight: 15.5 };
    expect(pixelToCell(7.8, 15.4, frac)).toEqual({ col: 0, row: 0 });
    expect(pixelToCell(7.82, 15.5, frac)).toEqual({ col: 1, row: 1 });
  });
});

describe("paneAtCell", () => {
  it("maps interior cells to their pane", () => {
    expect(paneAtCell(0, 0, PANES)?.id).toBe(0);
    expect(paneAtCell(59, 39, PANES)?.id).toBe(0);
    expect(paneAtCell(61, 0, PANES)?.id).toBe(1);
    expect(paneAtCell(119, 19, PANES)?.id).toBe(1);
    expect(paneAtCell(61, 21, PANES)?.id).toBe(2);
    expect(paneAtCell(119, 39, PANES)?.id).toBe(2);
  });
  it("divider cells belong to no pane", () => {
    expect(paneAtCell(60, 0, PANES)).toBeNull(); // vertical divider
    expect(paneAtCell(60, 39, PANES)).toBeNull();
    expect(paneAtCell(61, 20, PANES)).toBeNull(); // horizontal divider
    expect(paneAtCell(119, 20, PANES)).toBeNull();
  });
  it("outside the view is no pane", () => {
    expect(paneAtCell(120, 0, PANES)).toBeNull();
    expect(paneAtCell(0, 40, PANES)).toBeNull();
    expect(paneAtCell(-1, 0, PANES)).toBeNull();
  });
  it("boundary: first and last cell of each pane edge", () => {
    // Exclusive right/bottom edges — off-by-one here misroutes clicks.
    expect(paneAtCell(60 - 1, 0, PANES)?.id).toBe(0);
    expect(paneAtCell(61 + 59 - 1, 0, PANES)?.id).toBe(1);
    expect(paneAtCell(61, 21 + 19 - 1, PANES)?.id).toBe(2);
  });
});

describe("paneAtPixel", () => {
  const m = { cellWidth: 8, cellHeight: 16 };
  it("full path: pixel to pane id", () => {
    expect(paneAtPixel(0, 0, m, PANES)).toBe(0);
    expect(paneAtPixel(61 * 8, 0, m, PANES)).toBe(1);
    expect(paneAtPixel(61 * 8, 21 * 16, m, PANES)).toBe(2);
    expect(paneAtPixel(60 * 8, 0, m, PANES)).toBeNull(); // divider click
  });
});
