// Pixel→cell→pane mapping for mouse interaction. Isolated and pure:
// this is where DPI/font-metric drift bugs would live, so it is the
// unit-tested module (design: app/design/ux-spec.md).

import type { PaneRect } from "./transport";

export interface CellMetrics {
  /** Rendered width of one terminal cell in CSS pixels. */
  cellWidth: number;
  /** Rendered height of one terminal cell in CSS pixels. */
  cellHeight: number;
}

/** Convert a pixel position (relative to the terminal viewport's
 * top-left) to a cell coordinate. Fractional cells floor — a click
 * anywhere inside a cell belongs to that cell. */
export function pixelToCell(
  px: number,
  py: number,
  m: CellMetrics,
): { col: number; row: number } {
  return {
    col: Math.floor(px / m.cellWidth),
    row: Math.floor(py / m.cellHeight),
  };
}

/** Find the pane containing a cell. Rects come from MSG_LAYOUT and do
 * not overlap; divider cells (between panes) belong to no pane and
 * return null — a click on a divider must not steal focus. */
export function paneAtCell(col: number, row: number, panes: PaneRect[]): PaneRect | null {
  for (const p of panes) {
    if (col >= p.x && col < p.x + p.cols && row >= p.y && row < p.y + p.rows) {
      return p;
    }
  }
  return null;
}

/** One-shot: pixel position → pane id (or null on a divider/outside). */
export function paneAtPixel(
  px: number,
  py: number,
  m: CellMetrics,
  panes: PaneRect[],
): number | null {
  const { col, row } = pixelToCell(px, py, m);
  const pane = paneAtCell(col, row, panes);
  return pane ? pane.id : null;
}
