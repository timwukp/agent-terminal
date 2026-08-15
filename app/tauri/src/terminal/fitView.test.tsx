// @vitest-environment jsdom
//
// The view-fitting invariant, at the layer where it broke: xterm maps a
// pointer position to a cell by dividing a *visual* pixel offset (from
// element.getBoundingClientRect) by its own *unscaled* cell width, so any
// visual scaling of the terminal element multiplies the numerator and not
// the denominator. Measured in Chromium at scale 0.6 with the app's own
// xterm options: a drag across `bravo` selected "a b", the native copy
// event carried "a b", and a click on cell (40,10) reported (25,7) to the
// session. `transform: scale()` and CSS `zoom` failed identically.
//
// Those numbers cannot be reproduced here — jsdom has no layout engine, so
// it cannot mis-map anything. What IS checkable here, and is what a future
// "just scale it to fit" edit would break, is the invariant that the
// terminal element carries no visual scaling at any window size, plus the
// two consequences that make 1:1 usable: a clipped view scrolls to the
// bottom (where the prompt is) and says so, and the pane overlay follows
// the scroll instead of drifting off the panes it outlines.
//
// The browser-level numbers live in docs/UAT.md (GUI-34) so they are not
// lost with the harness that produced them.

import { describe, expect, it, vi, afterEach } from "vitest";
import { render, act, cleanup, fireEvent } from "@testing-library/react";
import TerminalView from "./Terminal";
import type { PaneRect } from "./transport";

const CELL = { width: 8, height: 16 };

// Read live by the getters installed in open(), so a test can change the
// geometry and fire a resize the way the window manager would.
const px = { termW: 626, termH: 360, hostW: 900, hostH: 600 };

const dom: { host: HTMLElement | null; el: HTMLElement | null } = {
  host: null,
  el: null,
};

vi.mock("@xterm/xterm", () => {
  class FakeTerm {
    element: HTMLElement | null = null;
    cols = 80;
    rows = 24;
    options: { fontSize?: number } = { fontSize: 13 };
    _core = { _renderService: { dimensions: { css: { cell: CELL } } } };
    buffer = {
      active: { length: 1, getLine: () => ({ translateToString: () => "" }) },
    };
    open(host: HTMLElement) {
      const el = document.createElement("div");
      el.appendChild(document.createElement("textarea"));
      host.appendChild(el);
      this.element = el;
      // jsdom performs no layout: every box is 0×0 and scrollTop is fixed
      // at 0. These stand in for the layout engine — the element reports
      // the grid's rendered size, the host reports the window's, and
      // scrollTop records what the component writes (jsdom cannot clamp
      // it, so what comes back out is exactly the component's decision).
      Object.defineProperty(el, "offsetWidth", {
        get: () => px.termW,
        configurable: true,
      });
      Object.defineProperty(el, "offsetHeight", {
        get: () => px.termH,
        configurable: true,
      });
      Object.defineProperty(host, "clientWidth", {
        get: () => px.hostW,
        configurable: true,
      });
      Object.defineProperty(host, "clientHeight", {
        get: () => px.hostH,
        configurable: true,
      });
      let top = 0;
      Object.defineProperty(host, "scrollTop", {
        get: () => top,
        set: (v: number) => {
          top = v;
        },
        configurable: true,
      });
      dom.host = host;
      dom.el = el;
    }
    focus() {}
    write() {}
    resize(cols: number, rows: number) {
      this.cols = cols;
      this.rows = rows;
    }
    scrollToBottom() {}
    dispose() {}
    onData() {
      return { dispose() {} };
    }
    onBell() {
      return { dispose() {} };
    }
    attachCustomKeyEventHandler() {}
    onScroll() {
      return { dispose() {} };
    }
    onWriteParsed() {
      return { dispose() {} };
    }
  }
  return { Terminal: FakeTerm };
});
vi.mock("@xterm/xterm/css/xterm.css", () => ({}));

type Ctrl = (ev: unknown) => void;
type Snap = (cols: number, rows: number, sb: number, blob: string) => void;

function makeTransport() {
  let ctrl: Ctrl = () => {};
  let snap: Snap = () => {};
  const t = {
    attach: vi.fn(
      (
        _s: string,
        _c: number,
        _r: number,
        ev: { onCtrl: Ctrl; onSnapshot: Snap },
      ) => {
        ctrl = ev.onCtrl;
        snap = ev.onSnapshot;
        return Promise.resolve();
      },
    ),
    stdin: () => Promise.resolve(),
    resize: vi.fn(() => Promise.resolve()),
    selectPane: () => Promise.resolve(),
    zoomToggle: () => Promise.resolve(),
    splitPane: () => Promise.resolve(),
    closePane: () => Promise.resolve(),
    scrollbackReq: () => Promise.resolve(),
    detach: () => Promise.resolve(),
  };
  return {
    t,
    snapshot: (cols: number, rows: number) => snap(cols, rows, 0, ""),
    layout: (panes: PaneRect[], activeId = 0, cols = 80, rows = 24) =>
      ctrl({
        kind: "layout",
        panes,
        active_id: activeId,
        view_cols: cols,
        view_rows: rows,
      }),
  };
}

async function mount() {
  const h = makeTransport();
  const view = render(
    <TerminalView transport={h.t as never} session="s" onClosed={() => {}} />,
  );
  await act(async () => {}); // let attach resolve and hand over the sinks
  return { ...h, view };
}

afterEach(() => {
  cleanup();
  vi.clearAllMocks();
  px.termW = 626;
  px.termH = 360;
  px.hostW = 900;
  px.hostH = 600;
});

describe("the terminal is never visually scaled", () => {
  it("carries no transform or zoom when the grid is larger than the window", async () => {
    px.hostW = 400;
    px.hostH = 200;
    const { snapshot } = await mount();
    await act(async () => snapshot(80, 24));
    // 626×360 of grid inside a 400×200 window: the exact configuration the
    // old code answered with scale(0.555).
    expect(dom.el!.style.transform).toBe("");
    expect(dom.el!.style.zoom).toBe("");
  });

  it("carries no transform or zoom when the window is much larger", async () => {
    px.hostW = 1600;
    px.hostH = 1000;
    const { snapshot } = await mount();
    await act(async () => snapshot(80, 24));
    // The old code clamped upscaling at 1:1 by writing scale(1); a written
    // scale(1) would still fail this, and it should — the property under
    // test is "no scaling code runs", not "the factor happens to be 1".
    expect(dom.el!.style.transform).toBe("");
    expect(dom.el!.style.zoom).toBe("");
  });

  it("still carries none after the window is resized down to a sliver", async () => {
    const { snapshot } = await mount();
    await act(async () => snapshot(80, 24));
    px.hostW = 120;
    px.hostH = 80;
    await act(async () => void fireEvent(window, new Event("resize")));
    expect(dom.el!.style.transform).toBe("");
    expect(dom.el!.style.zoom).toBe("");
  });

  it("keeps the host scrollable, so a clipped session stays reachable", async () => {
    const { snapshot } = await mount();
    await act(async () => snapshot(80, 24));
    expect(dom.host!.style.overflow).toBe("auto");
  });
});

describe("a clipped view", () => {
  it("is scrolled to the bottom, where the prompt is", async () => {
    px.hostH = 200;
    const { snapshot } = await mount();
    await act(async () => snapshot(80, 24));
    expect(dom.host!.scrollTop).toBe(360 - 200);
  });

  it("stays at the top when the whole grid fits", async () => {
    const { snapshot } = await mount();
    await act(async () => snapshot(80, 24));
    expect(dom.host!.scrollTop).toBe(0);
  });

  it("says so, naming the session's grid", async () => {
    px.hostW = 400;
    px.hostH = 200;
    const { snapshot, view } = await mount();
    await act(async () => snapshot(111, 54));
    const hint = view.getByText(/scroll to see it all/);
    expect(hint.textContent).toContain("111×54");
    expect(hint.getAttribute("title")).toMatch(/wrong cell/);
  });
});

describe("the fit hint", () => {
  // The hint used to be computed only inside the "did the grid change?"
  // branch of onSnapshot, so a session that was already exactly 80×24 got
  // no hint at all — the one case where the hint is the only explanation
  // on screen.
  it("appears for a session whose size the snapshot did not change", async () => {
    px.hostW = 1600;
    px.hostH = 1000;
    const { snapshot, view } = await mount();
    await act(async () => snapshot(80, 24));
    expect(view.getByText(/empty space/).textContent).toContain("80×24");
  });

  it("is absent when window and grid nearly agree", async () => {
    px.hostW = 640;
    px.hostH = 400;
    const { snapshot, view } = await mount();
    await act(async () => snapshot(80, 24));
    expect(view.queryByText(/empty space/)).toBeNull();
    expect(view.queryByText(/scroll to see it all/)).toBeNull();
  });

  it("prefers 'clipped' over 'empty space' for a wide, short grid", async () => {
    // Both conditions hold: 1200 wide in a 900 window (clipped) and only
    // 120 tall in a 600 one (85% dead space).
    px.termW = 1200;
    px.termH = 120;
    const { snapshot, view } = await mount();
    await act(async () => snapshot(150, 8));
    expect(view.queryByText(/empty space/)).toBeNull();
    expect(view.getByText(/scroll to see it all/)).toBeTruthy();
  });
});

describe("the pane overlay follows a panned view", () => {
  const TILED: PaneRect[] = [
    { id: 0, x: 0, y: 0, cols: 40, rows: 24 },
    { id: 1, x: 41, y: 0, cols: 39, rows: 24 },
  ];

  it("is offset by the host's scroll, so boxes stay on their panes", async () => {
    px.hostW = 400;
    px.hostH = 200;
    const { snapshot, layout, view } = await mount();
    await act(async () => snapshot(80, 24));
    await act(async () => layout(TILED, 1));
    // Pinned to the bottom by the fit above: 360 − 200.
    const pan = view.getByTestId("pane-overlay-pan");
    expect(pan.style.top).toBe("-160px");
    expect(pan.style.left).toBe("0px");
    // Now the user pans sideways with the scrollbar.
    dom.host!.scrollLeft = 90;
    await act(async () => void fireEvent.scroll(dom.host!));
    expect(view.getByTestId("pane-overlay-pan").style.left).toBe("-90px");
    // The boxes themselves stay in grid coordinates — the offset is applied
    // once, to their container, which is why they cannot disagree.
    const right = view
      .getByTestId("pane-overlay")
      .querySelector('[data-pane-id="1"]');
    expect((right as HTMLElement).style.left).toBe(`${41 * CELL.width}px`);
    // jsdom clips nothing, so this asserts only that the property is set —
    // that the boxes of a grid wider than the window really stop at the
    // window edge instead of being drawn over the sidebar is a browser
    // fact, checked by eye in UAT (GUI-34).
    expect(view.getByTestId("pane-overlay").style.overflow).toBe("hidden");
  });
});
