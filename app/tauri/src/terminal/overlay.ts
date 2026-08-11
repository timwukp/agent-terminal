// Cell rect → pixel rect, the inverse of hittest.ts.
//
// Kept pure and separate for the same reason hittest.ts is: this is where
// DPI and font-metric drift would live. Having the inverse available is
// what makes the transform falsifiable — a wrong cell size shows up as a
// visibly misaligned overlay instead of as a click selecting the wrong
// pane, and the round-trip (cell → pixels → cell) is testable without a
// browser.

import type { PaneRect } from "./transport";
import type { CellMetrics } from "./hittest";

export interface PixelRect {
  left: number;
  top: number;
  width: number;
  height: number;
}

/** Place a pane's cell rect in unscaled CSS pixels, relative to the
 * terminal element's content origin. Unscaled on purpose: the caller
 * applies the same `scale()` transform xterm's element carries, so cell
 * sizes stay in the units xterm reports. */
export function paneRectToPixels(pane: PaneRect, m: CellMetrics): PixelRect {
  return {
    left: pane.x * m.cellWidth,
    top: pane.y * m.cellHeight,
    width: pane.cols * m.cellWidth,
    height: pane.rows * m.cellHeight,
  };
}

/** Cell metrics from xterm's render service.
 *
 * `_core` is private API. It is used because xterm exposes no public cell
 * measurement, and the alternative — assuming a cell size — is what
 * silently breaks under a different font or DPI. Returns null rather than
 * throwing when the shape is not what we expect, so a version bump
 * degrades to "no overlay" instead of a blank window.
 */
export function readCellMetrics(term: unknown): CellMetrics | null {
  const cell = (
    term as {
      _core?: {
        _renderService?: {
          dimensions?: { css?: { cell?: { width?: unknown; height?: unknown } } };
        };
      };
    }
  )?._core?._renderService?.dimensions?.css?.cell;
  const width = cell?.width;
  const height = cell?.height;
  if (typeof width !== "number" || typeof height !== "number") return null;
  if (!(width > 0) || !(height > 0)) return null; // also rejects NaN
  return { cellWidth: width, cellHeight: height };
}
