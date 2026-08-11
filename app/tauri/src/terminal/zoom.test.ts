import { describe, expect, it } from "vitest";
import { zoomedPaneId } from "./zoom";
import type { PaneRect } from "./transport";

// Real geometry, captured from the daemon on 2026-08-11 (isolated run,
// 80x24 session, MSG_SPLIT_PANE then MSG_SELECT_PANE mode 8): the same
// sequence the integration test zoom_is_visible_in_layout_geometry drives.
const TILED: PaneRect[] = [
  { id: 0, x: 0, y: 0, cols: 40, rows: 24 },
  { id: 1, x: 41, y: 0, cols: 39, rows: 24 },
];
const ZOOMED: PaneRect[] = [
  { id: 0, x: 0, y: 0, cols: 40, rows: 24 }, // keeps its tree rect
  { id: 1, x: 0, y: 0, cols: 80, rows: 24 }, // covers the whole view
];

describe("zoomedPaneId", () => {
  it("tiled split is not zoom", () => {
    expect(zoomedPaneId(TILED, 80, 24)).toBeNull();
  });

  it("the pane covering the whole view is the zoomed one", () => {
    expect(zoomedPaneId(ZOOMED, 80, 24)).toBe(1);
  });

  it("a single pane always fills the view and is never 'zoomed'", () => {
    expect(zoomedPaneId([{ id: 0, x: 0, y: 0, cols: 80, rows: 24 }], 80, 24)).toBeNull();
  });

  it("two full-view panes report nothing rather than picking one", () => {
    // The daemon never sends this today; if it ever does, the geometric
    // assumption behind this module is broken and a wrong-but-confident
    // answer is worse than none. The integration test cannot cover this
    // case for the same reason — the daemon won't produce it.
    const both: PaneRect[] = [
      { id: 0, x: 0, y: 0, cols: 80, rows: 24 },
      { id: 1, x: 0, y: 0, cols: 80, rows: 24 },
    ];
    expect(zoomedPaneId(both, 80, 24)).toBeNull();
  });

  it("degenerate view dimensions report nothing", () => {
    // A 0x0 view would make every pane 'cover' it vacuously.
    expect(zoomedPaneId(ZOOMED, 0, 0)).toBeNull();
  });

  it("a pane matching only width or only height is not zoomed", () => {
    // A stacked split's top pane spans the full width at y=0; that must
    // not read as zoom just because two of four numbers match.
    const stacked: PaneRect[] = [
      { id: 0, x: 0, y: 0, cols: 80, rows: 12 },
      { id: 1, x: 0, y: 13, cols: 80, rows: 11 },
    ];
    expect(zoomedPaneId(stacked, 80, 24)).toBeNull();
  });
});
