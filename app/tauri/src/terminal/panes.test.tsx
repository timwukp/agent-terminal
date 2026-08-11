// @vitest-environment jsdom
//
// The pane UI wiring: layout events in, overlay boxes and toolbar actions
// out. The geometry math has its own suites (overlay.test.ts, zoom.test.ts,
// hittest.test.ts); what is under test here is the part none of them see —
// that TerminalView actually routes a MSG_LAYOUT into an overlay and a
// toolbar click into a transport call. The first pane feature shipped with
// working math and was still unverifiable by eye for two rounds; this is
// the layer where that gap lived.

import { describe, expect, it, vi, afterEach } from "vitest";
import { render, fireEvent, act, cleanup } from "@testing-library/react";
import TerminalView from "./Terminal";
import type { PaneRect } from "./transport";

const CELL = { width: 8, height: 16 };

vi.mock("@xterm/xterm", () => {
  class FakeTerm {
    element: HTMLElement | null = null;
    cols = 80;
    rows = 24;
    // The real render service, minimally: readCellMetrics() reads this
    // exact shape, and the overlay only renders when it is present.
    _core = { _renderService: { dimensions: { css: { cell: CELL } } } };
    open(host: HTMLElement) {
      const el = document.createElement("div");
      el.appendChild(document.createElement("textarea"));
      host.appendChild(el);
      this.element = el;
    }
    focus() {
      this.element?.querySelector("textarea")?.focus();
    }
    write() {}
    resize() {}
    scrollToBottom() {}
    dispose() {}
    onData() {
      return { dispose() {} };
    }
  }
  return { Terminal: FakeTerm };
});
vi.mock("@xterm/xterm/css/xterm.css", () => ({}));

type Ctrl = (ev: unknown) => void;

/** Transport double that hands the test the onCtrl callback, so a test can
 * play the daemon and push layout events. */
function makeTransport() {
  let ctrl: Ctrl = () => {};
  const t = {
    attach: vi.fn((_s: string, _c: number, _r: number, events: { onCtrl: Ctrl }) => {
      ctrl = events.onCtrl;
      return Promise.resolve();
    }),
    stdin: vi.fn(() => Promise.resolve()),
    resize: vi.fn(() => Promise.resolve()),
    selectPane: vi.fn(() => Promise.resolve()),
    zoomToggle: vi.fn(() => Promise.resolve()),
    splitPane: vi.fn(() => Promise.resolve()),
    closePane: vi.fn(() => Promise.resolve()),
    detach: vi.fn(() => Promise.resolve()),
  };
  return { t, layout: (panes: PaneRect[], activeId = 0, cols = 80, rows = 24) => ctrl({ kind: "layout", panes, active_id: activeId, view_cols: cols, view_rows: rows }) };
}

const TILED: PaneRect[] = [
  { id: 0, x: 0, y: 0, cols: 40, rows: 24 },
  { id: 1, x: 41, y: 0, cols: 39, rows: 24 },
];
const ZOOMED: PaneRect[] = [
  { id: 0, x: 0, y: 0, cols: 40, rows: 24 },
  { id: 1, x: 0, y: 0, cols: 80, rows: 24 },
];

async function mount() {
  const { t, layout } = makeTransport();
  const view = render(
    <TerminalView transport={t as never} session="s" onClosed={() => {}} />,
  );
  await act(async () => {}); // let attach resolve and hand over onCtrl
  return { t, layout, view };
}

// cleanup() as well as clearAllMocks: without it renders accumulate in one
// document and every by-title query finds N toolbars (focus.test.tsx hit
// the same trap).
afterEach(() => {
  cleanup();
  vi.clearAllMocks();
});

describe("pane overlay", () => {
  it("is absent for a single pane", async () => {
    const { layout, view } = await mount();
    await act(async () => layout([{ id: 0, x: 0, y: 0, cols: 80, rows: 24 }]));
    expect(view.queryByTestId("pane-overlay")).toBeNull();
  });

  it("draws one box per pane at the layout's geometry, active marked", async () => {
    const { layout, view } = await mount();
    await act(async () => layout(TILED, 1));
    const overlay = view.getByTestId("pane-overlay");
    const boxes = overlay.querySelectorAll("[data-pane-id]");
    expect(boxes).toHaveLength(2);
    const right = overlay.querySelector('[data-pane-id="1"]') as HTMLElement;
    expect(right.dataset.active).toBe("true");
    // Geometry flows from the layout through paneRectToPixels: pane 1
    // starts at cell column 41 with 8px cells.
    expect(right.style.left).toBe(`${41 * CELL.width}px`);
    const left = overlay.querySelector('[data-pane-id="0"]') as HTMLElement;
    expect(left.dataset.active).toBe("false");
  });

  it("moves the highlight when a new layout changes the active pane", async () => {
    const { layout, view } = await mount();
    await act(async () => layout(TILED, 1));
    await act(async () => layout(TILED, 0));
    const overlay = view.getByTestId("pane-overlay");
    expect((overlay.querySelector('[data-pane-id="0"]') as HTMLElement).dataset.active).toBe(
      "true",
    );
    expect((overlay.querySelector('[data-pane-id="1"]') as HTMLElement).dataset.active).toBe(
      "false",
    );
  });
});

describe("pane toolbar", () => {
  it("split buttons send the split orientation on the transport", async () => {
    const { t, view } = await mount();
    fireEvent.click(view.getByTitle("Split left/right"));
    expect(t.splitPane).toHaveBeenCalledWith(false);
    fireEvent.click(view.getByTitle("Split top/bottom"));
    expect(t.splitPane).toHaveBeenCalledWith(true);
  });

  it("close is hidden at one pane and sends close_pane at two", async () => {
    const { t, layout, view } = await mount();
    expect(view.queryByTitle("Close active pane")).toBeNull();
    await act(async () => layout(TILED, 1));
    fireEvent.click(view.getByTitle("Close active pane"));
    expect(t.closePane).toHaveBeenCalled();
  });

  it("zoom button reflects zoom state derived from geometry", async () => {
    const { t, layout, view } = await mount();
    await act(async () => layout(TILED, 1));
    const zoomBtn = view.getByTitle("Zoom active pane");
    expect(zoomBtn.getAttribute("aria-pressed")).toBe("false");
    fireEvent.click(zoomBtn);
    expect(t.zoomToggle).toHaveBeenCalled();
    // The daemon answers with a layout whose zoomed pane covers the view
    // (verified against the real daemon in zoom_is_visible_in_layout_geometry).
    await act(async () => layout(ZOOMED, 1));
    expect(view.getByTitle("Unzoom pane").getAttribute("aria-pressed")).toBe("true");
  });
});
