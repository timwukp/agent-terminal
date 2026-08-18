// @vitest-environment jsdom
//
// The deep-history seam, tested where it can actually go wrong.
//
// historyView.test.ts already pins the machine's decisions. What it cannot
// see is the part that only exists in Terminal.tsx: MSG_SCROLLBACK_DATA has
// no requester field, so ONE frame type feeds TWO consumers — the
// attach-time backfill and, later, the viewer. Route a page to the wrong
// one and the symptom is not an exception: backfill silently drops it (it
// is live by then) and the viewer sits stalled on screen, or history lands
// in the LIVE terminal, which is corruption the user reads as a product
// bug. So the assertions here are about which xterm received which bytes.
//
// The other thing only this layer knows is where the live buffer actually
// begins. `sb_lines - FETCH_MAX` is where we ASKED from; the daemon answers
// from the oldest seq the log still holds, which rotation moves forward.
// A viewer anchored to the request would leave a hole and present it as
// continuous, so the rotated case is a test, not a footnote.

import { afterEach, describe, expect, it, vi } from "vitest";
import { render, act, cleanup, fireEvent } from "@testing-library/react";
import TerminalView from "./Terminal";
import { FETCH_MAX, PAGE_LINES as BACKFILL_PAGE } from "./backfill";
import { WINDOW_LINES, PAGE_LINES } from "./historyView";

const CELL = { width: 8, height: 16 };

interface Fake {
  writes: string[];
  resets: number;
  scrolledTo: Array<[number, string]>;
  disposed: boolean;
  buf: { viewportY: number; baseY: number };
  fireScroll: () => void;
  cols: number;
  rows: number;
  /** Buffer rows each written line occupies. 1 = nothing wraps; 2 = every
   * stored line is twice the viewer's width, which is what a session resized
   * narrower since produces. */
  wrap: number;
}

// Instance 0 is the live terminal; instance 1, when present, is the
// viewer's. Keeping them apart is the whole point of this file.
const terms: Fake[] = [];

vi.mock("@xterm/xterm", () => {
  class FakeTerm {
    element: HTMLElement | null = null;
    cols = 80;
    rows = 24;
    options: Record<string, unknown> = { fontSize: 13 };
    _core = { _renderService: { dimensions: { css: { cell: CELL } } } };
    private scrollCbs: Array<() => void> = [];
    private pending: Array<{ data: string; cb?: () => void }> = [];
    private scheduled = false;
    private self: Fake;
    constructor() {
      const t = this;
      this.self = {
        writes: [],
        resets: 0,
        scrolledTo: [],
        disposed: false,
        buf: { viewportY: 0, baseY: 0 },
        wrap: 1,
        fireScroll: () => t.scrollCbs.forEach((c) => c()),
        get cols() {
          return t.cols;
        },
        get rows() {
          return t.rows;
        },
      } as Fake;
      terms.push(this.self);
    }
    get buffer() {
      // Rows, not lines: each written line occupies `wrap` of them, and only
      // the first is unwrapped. This is the one xterm behaviour the row
      // mapping in HistoryOverlay depends on, so the fake has to have it —
      // a fake reporting one row per line would agree with the arithmetic
      // the mapping exists to replace.
      const written = this.self.writes.filter((w) => w.endsWith("\r\n")).length;
      const wrap = this.self.wrap;
      return {
        active: {
          viewportY: this.self.buf.viewportY,
          baseY: this.self.buf.baseY,
          length: Math.max(1, written * wrap),
          getLine: (row: number) => ({
            isWrapped: wrap > 1 && row % wrap !== 0,
            translateToString: () => "",
          }),
        },
      };
    }
    open(host: HTMLElement) {
      const el = document.createElement("div");
      el.appendChild(document.createElement("textarea"));
      host.appendChild(el);
      this.element = el;
      Object.defineProperty(el, "offsetWidth", { get: () => 640, configurable: true });
      Object.defineProperty(el, "offsetHeight", { get: () => 400, configurable: true });
      Object.defineProperty(host, "clientWidth", { get: () => 900, configurable: true });
      Object.defineProperty(host, "clientHeight", { get: () => 600, configurable: true });
    }
    // xterm parses on its own queue: `write` returns immediately, having
    // scheduled the work (WriteBuffer.write -> setTimeout), the callback runs
    // only after every chunk queued ahead of it, and each line that scrolls
    // off fires onScroll with the viewport pinned to the bottom
    // (BufferService.scroll -> `ydisp = ybase`, then `_onScroll.fire`).
    //
    // A fake that parsed synchronously would deliver those scroll events
    // INSIDE the machine's own onPage — where `loading` is still true and
    // every edge handler is guarded — and so could never show what the real
    // one does: the events land after the last page has already cleared
    // `loading`. Measured on @xterm/headless 6.0.0, the same version as the
    // app's @xterm/xterm: 100 queued lines produce 0 scroll events before the
    // drain and 71 after it, all of them with viewportY >= baseY.
    write(data: string | Uint8Array, cb?: () => void) {
      // Decoded, not counted: the assertions are about WHICH lines reached
      // WHICH terminal, and history arrives as bytes while the SGR
      // bracketing around it arrives as strings.
      this.pending.push({
        data: typeof data === "string" ? data : new TextDecoder().decode(data),
        cb,
      });
      if (this.scheduled) return;
      this.scheduled = true;
      queueMicrotask(() => this.drain());
    }
    private drain() {
      this.scheduled = false;
      const queued = this.pending;
      this.pending = [];
      let lines = 0;
      for (const e of queued) {
        this.self.writes.push(e.data);
        if (e.data.endsWith("\r\n")) lines++;
      }
      if (lines > 0) {
        // Coalesced into one event: what matters is WHEN it arrives relative
        // to the callbacks, not how many there are. The viewport follows the
        // bottom only when it was already there — xterm holds a
        // user-scrolled view still (`isUserScrolling`), which is the case the
        // live terminal's "you are scrolled up" pill depends on.
        const wasAtBottom = this.self.buf.viewportY >= this.self.buf.baseY;
        this.self.buf.baseY += lines;
        if (wasAtBottom) {
          this.self.buf.viewportY = this.self.buf.baseY;
          this.scrollCbs.forEach((c) => c());
        }
      }
      for (const e of queued) e.cb?.();
    }
    reset() {
      this.self.resets++;
      this.self.buf.baseY = 0;
      this.self.buf.viewportY = 0;
      // Real xterm fires exactly ONE onScroll from reset(), at [0, 0] —
      // measured on 6.0.0, both on a fresh terminal and on one holding 100
      // lines. It is the event that makes a re-anchor re-enter the machine.
      this.scrollCbs.forEach((c) => c());
    }
    scrollToLine(line: number) {
      this.self.scrolledTo.push([line, "line"]);
    }
    scrollLines(n: number) {
      this.self.scrolledTo.push([n, "lines"]);
    }
    scrollPages(n: number) {
      this.self.scrolledTo.push([n, "pages"]);
    }
    resize(cols: number, rows: number) {
      this.cols = cols;
      this.rows = rows;
    }
    focus() {}
    scrollToBottom() {}
    dispose() {
      this.self.disposed = true;
    }
    onData() {
      return { dispose() {} };
    }
    onBell() {
      return { dispose() {} };
    }
    attachCustomKeyEventHandler() {}
    onScroll(cb: () => void) {
      this.scrollCbs.push(cb);
      return { dispose: () => void (this.scrollCbs = this.scrollCbs.filter((c) => c !== cb)) };
    }
    onWriteParsed(cb: () => void) {
      this.scrollCbs.push(cb);
      return { dispose() {} };
    }
  }
  return { Terminal: FakeTerm };
});
vi.mock("@xterm/xterm/css/xterm.css", () => ({}));

type Snap = (cols: number, rows: number, sb: number, blob: Uint8Array) => void;
type Sb = (firstSeq: number, lines: Uint8Array[]) => void;

const line = (seq: number) => new TextEncoder().encode(`s${seq}`);
const pageOf = (start: number, n: number) => Array.from({ length: n }, (_, i) => line(start + i));

function harness() {
  let snap: Snap = () => {};
  let sb: Sb = () => {};
  const req: Array<[number, number]> = [];
  const transport = {
    attach: vi.fn((_s: string, _c: number, _r: number, ev: { onSnapshot: Snap; onScrollback: Sb }) => {
      snap = ev.onSnapshot;
      sb = ev.onScrollback;
      return Promise.resolve();
    }),
    stdin: () => Promise.resolve(),
    resize: () => Promise.resolve(),
    selectPane: () => Promise.resolve(),
    zoomToggle: () => Promise.resolve(),
    splitPane: () => Promise.resolve(),
    closePane: () => Promise.resolve(),
    scrollbackReq: vi.fn((start: number, max: number) => {
      req.push([start, max]);
      return Promise.resolve();
    }),
    detach: () => Promise.resolve(),
  };
  return {
    transport,
    req,
    snapshot: (cols: number, rows: number, sbLines: number) =>
      snap(cols, rows, sbLines, new TextEncoder().encode("repaint")),
    serve: (firstSeq: number, n: number) => sb(firstSeq, pageOf(firstSeq, n)),
  };
}

/** Attach, snapshot, and let the attach-time backfill run to completion —
 * answering each of its pages from `firstServed` onward, which is how a
 * rotated log behaves. Returns the seq of the oldest line the live buffer
 * ended up holding. */
async function attachAndBackfill(h: ReturnType<typeof harness>, total: number, firstServed: number) {
  const view = render(
    <TerminalView transport={h.transport as never} session="claude" autoFit={() => false} />,
  );
  await act(async () => {});
  await act(async () => {
    h.snapshot(124, 30, total);
  });
  // The first ask is the requested floor; the answer may be later.
  expect(h.req[0][0]).toBe(Math.max(0, total - FETCH_MAX));
  let served = firstServed;
  // All 25 pages inside ONE act: the backfill machine is synchronous, so
  // serving page N+1 needs no React flush after page N — and 25 flushes of
  // a component holding 25,000 written lines is most of this file's runtime.
  // Bounded by a guard rather than by `while (h.req.length)`, so a routing
  // bug that re-asks forever fails here instead of hanging the run.
  await act(async () => {
    for (let guard = 0; guard < 64; guard++) {
      const [start, max] = h.req[h.req.length - 1];
      const from = Math.max(start, served);
      const n = Math.max(0, Math.min(max, total - from));
      const before = h.req.length;
      h.serve(from, n);
      served = from + n;
      if (h.req.length === before) break; // backfill went live
    }
  });
  return view;
}

/** Scroll the live terminal to the top of its buffer, which is where the
 * "open history" offer lives. */
async function scrollLiveToTop(atTop = true) {
  await act(async () => {
    terms[0].buf.viewportY = atTop ? 0 : 500;
    terms[0].buf.baseY = 1000;
    terms[0].fireScroll();
  });
}

afterEach(() => {
  cleanup();
  terms.length = 0;
  vi.clearAllMocks();
});

describe("deep history", () => {
  it("offers the unreached lines by count, at the top of the buffer", async () => {
    const h = harness();
    const view = await attachAndBackfill(h, 93374, 93374 - FETCH_MAX);
    // Not offered while the user is mid-buffer: the offer explains a
    // boundary, and away from the boundary it is just noise.
    await scrollLiveToTop(false);
    expect(view.queryByRole("button", { name: /open history/ })).toBeNull();
    await scrollLiveToTop();
    const pill = view.getByRole("button", { name: /open history/ });
    // 93,374 - 25,000: the exact number of lines the live terminal cannot
    // hold, which is the number the reporter was owed.
    expect(pill.textContent).toContain("68,374");
    expect(pill.getAttribute("title")).toContain("93,374");
    // …and the complement, so the tooltip's two numbers add up to the log
    // rather than being independently plausible.
    expect(pill.getAttribute("title")).toContain(FETCH_MAX.toLocaleString("en-US"));
    // The wording matters as much as the number — the reading it exists to
    // rule out is "the session was restarted / its memory was cleared".
    expect(pill.getAttribute("title")).toMatch(/not restarted/i);
  });

  it("does not invent a boundary when the backfill wrote nothing", async () => {
    const h = harness();
    const view = render(
      <TerminalView transport={h.transport as never} session="claude" autoFit={() => false} />,
    );
    await act(async () => {});
    await act(async () => {
      h.snapshot(124, 30, 93374);
    });
    // Rotation dropped everything the backfill asked for, so the daemon
    // answers with an empty page and the client never learns where its own
    // buffer begins. "Holds the newest 0 lines" would be false as soon as
    // live output scrolls in, so neither number may be presented as fact.
    await act(async () => {
      h.serve(93374 - FETCH_MAX, 0);
    });
    await scrollLiveToTop();
    const pill = view.getByRole("button", { name: /open history/ });
    expect(pill.textContent).not.toMatch(/[0-9]/);
    expect(pill.getAttribute("title")).toMatch(/unknown/i);
    // The one number that IS known — the log's size — is still shown, and so
    // is the reading the whole pill exists to rule out.
    expect(pill.getAttribute("title")).toContain("93,374");
    expect(pill.getAttribute("title")).toMatch(/not restarted/i);
    h.req.length = 0;
    await act(async () => {
      fireEvent.click(pill);
    });
    // With no observed boundary the only honest window is the newest one.
    expect(h.req[0]).toEqual([93374 - WINDOW_LINES, PAGE_LINES]);
  });

  it("does not offer the viewer while the attach fetch is still running", async () => {
    const h = harness();
    const view = render(
      <TerminalView transport={h.transport as never} session="claude" autoFit={() => false} />,
    );
    await act(async () => {});
    await act(async () => {
      h.snapshot(124, 30, 93374);
    });
    // One page of 25 has landed. The count is already knowable (oldestWritten
    // is set) and the user can be at the top of the buffer right now, so the
    // offer is *reachable* here — and taking it would put a second consumer on
    // MSG_SCROLLBACK_DATA while 24 pages are still in flight. The viewer would
    // eat page 2, anchor on a seq nobody chose, and the live terminal would
    // stall and paint 1,000 lines of history instead of 25,000.
    await act(async () => {
      h.serve(93374 - FETCH_MAX, BACKFILL_PAGE);
    });
    await scrollLiveToTop();
    expect(view.queryByRole("button", { name: /open history/ })).toBeNull();
    // …and it is hidden because the fetch is unfinished, not because there is
    // nothing to offer: the backfill has already asked for its next page.
    expect(h.req.length).toBeGreaterThan(1);
    // Let the rest of the fetch finish; now the offer is safe and appears
    // without the user having to scroll again.
    let served = 93374 - FETCH_MAX + BACKFILL_PAGE;
    await act(async () => {
      for (let guard = 0; guard < 64; guard++) {
        const [start, max] = h.req[h.req.length - 1];
        const from = Math.max(start, served);
        const n = Math.max(0, Math.min(max, 93374 - from));
        const before = h.req.length;
        h.serve(from, n);
        served = from + n;
        if (h.req.length === before) break;
      }
    });
    expect(view.getByRole("button", { name: /open history/ }).textContent).toContain("68,374");
  });

  it("says nothing when the buffer holds the whole log", async () => {
    const h = harness();
    const view = await attachAndBackfill(h, 900, 0);
    await scrollLiveToTop();
    expect(view.queryByRole("button", { name: /open history/ })).toBeNull();
  });

  it("anchors the viewer to the line the daemon served, not the one we asked for", async () => {
    const h = harness();
    // Rotation: asked from 68,374, the log now starts at 70,000.
    const view = await attachAndBackfill(h, 93374, 70000);
    await scrollLiveToTop();
    expect(view.getByRole("button", { name: /open history/ }).textContent).toContain("70,000");
    h.req.length = 0;
    await act(async () => {
      fireEvent.click(view.getByRole("button", { name: /open history/ }));
    });
    // The window ENDS where the live buffer begins: [70000-4000, 70000).
    // Anchored to 68,374 instead, the last 1,626 lines of the window would
    // be lines the live buffer already had and the 1,626 before 70,000
    // would be missing from both views.
    expect(h.req[0]).toEqual([70000 - WINDOW_LINES, PAGE_LINES]);
  });

  it("routes a page to the open viewer and never to the live terminal", async () => {
    const h = harness();
    const view = await attachAndBackfill(h, 93374, 93374 - FETCH_MAX);
    await scrollLiveToTop();
    h.req.length = 0;
    await act(async () => {
      fireEvent.click(view.getByRole("button", { name: /open history/ }));
    });
    expect(terms.length).toBe(2); // a second, read-only terminal
    const liveWrites = terms[0].writes.length;
    const anchor = 93374 - FETCH_MAX - WINDOW_LINES;
    await act(async () => {
      for (let i = 0; i < WINDOW_LINES / PAGE_LINES; i++) {
        const [start, max] = h.req[h.req.length - 1];
        h.serve(start, max);
      }
    });
    // Every history line landed in the viewer's terminal…
    expect(terms[1].writes.filter((w) => w.startsWith("s")).length).toBe(WINDOW_LINES);
    // …and none of it in the session's.
    expect(terms[0].writes.length).toBe(liveWrites);
    expect(view.getByTestId("history-position").textContent).toBe(
      `${(anchor + 1).toLocaleString("en-US")}–${(anchor + WINDOW_LINES).toLocaleString("en-US")} of 93,374`,
    );
  });

  it("follows a later snapshot's line count while the viewer is open", async () => {
    const h = harness();
    const view = await attachAndBackfill(h, 93374, 93374 - FETCH_MAX);
    await scrollLiveToTop();
    h.req.length = 0;
    await act(async () => {
      fireEvent.click(view.getByRole("button", { name: /open history/ }));
    });
    await act(async () => {
      for (let i = 0; i < WINDOW_LINES / PAGE_LINES; i++) {
        const [start, max] = h.req[h.req.length - 1];
        h.serve(start, max);
      }
    });
    expect(view.getByTestId("history-position").textContent).toContain("of 93,374");
    // The session did not stop while the viewer was up: 1,626 more lines
    // scrolled off, so the log the position counts in is bigger now. Passing
    // the count captured at open time left this stale — and with it the
    // "newer history exists" decision that ⤓ and edge-paging are built on.
    await act(async () => {
      h.snapshot(124, 30, 95000);
    });
    expect(view.getByTestId("history-position").textContent).toContain("of 95,000");
  });

  it("Escape closes the viewer, frees its terminal, and restores the offer", async () => {
    const h = harness();
    const view = await attachAndBackfill(h, 93374, 93374 - FETCH_MAX);
    await scrollLiveToTop();
    await act(async () => {
      fireEvent.click(view.getByRole("button", { name: /open history/ }));
    });
    expect(view.queryByTestId("history-overlay")).not.toBeNull();
    await act(async () => {
      fireEvent.keyDown(view.getByTestId("history-overlay"), { key: "Escape" });
    });
    expect(view.queryByTestId("history-overlay")).toBeNull();
    // The window's cell storage is the cost of this feature; it is only
    // acceptable because closing gives it back.
    expect(terms[1].disposed).toBe(true);
    expect(terms[0].disposed).toBe(false);
    expect(view.queryByRole("button", { name: /open history/ })).not.toBeNull();
  });

  it("moves the window when the viewer is scrolled to an edge", async () => {
    const h = harness();
    const view = await attachAndBackfill(h, 93374, 93374 - FETCH_MAX);
    await scrollLiveToTop();
    await act(async () => {
      fireEvent.click(view.getByRole("button", { name: /open history/ }));
    });
    await act(async () => {
      for (let i = 0; i < WINDOW_LINES / PAGE_LINES; i++) {
        const [start, max] = h.req[h.req.length - 1];
        h.serve(start, max);
      }
    });
    const anchor = 93374 - FETCH_MAX - WINDOW_LINES;
    h.req.length = 0;
    // Wheel-scrolled to the top of the window: keep going.
    await act(async () => {
      terms[1].buf.viewportY = 0;
      terms[1].buf.baseY = WINDOW_LINES;
      terms[1].fireScroll();
    });
    expect(h.req[0]).toEqual([anchor - WINDOW_LINES / 2, PAGE_LINES]);
  });

  it("opens the window once, at the join, instead of walking itself to seq 0", async () => {
    const h = harness();
    const view = await attachAndBackfill(h, 93374, 93374 - FETCH_MAX);
    await scrollLiveToTop();
    h.req.length = 0;
    await act(async () => {
      fireEvent.click(view.getByRole("button", { name: /open history/ }));
    });
    await act(async () => {
      for (let i = 0; i < WINDOW_LINES / PAGE_LINES; i++) {
        const [start, max] = h.req[h.req.length - 1];
        h.serve(start, max);
      }
    });
    // A re-anchor empties the view before it asks for anything, and xterm's
    // reset() fires an onScroll at [0, 0] — the top edge. That event arrives
    // inside the seek that is still running, before `loading` is set, so the
    // machine's own guard cannot stop it: `earlier()` re-enters, re-anchors
    // half a window lower, resets again. Measured against the real xterm
    // 6.0.0, one open() of this session produced 34 nested seeks and 54 page
    // requests and left the window at the START of the log, showing lines
    // 3,507 down to 1,999 out of order — the user asks for the conversation
    // just above the live buffer and gets the oldest, scrambled.
    const anchor = 93374 - FETCH_MAX - WINDOW_LINES;
    expect(h.req).toEqual([
      [anchor, PAGE_LINES],
      [anchor + 1000, PAGE_LINES],
      [anchor + 2000, PAGE_LINES],
      [anchor + 3000, PAGE_LINES],
    ]);
    // One window, so one reset. This is the number that separates "opened"
    // from "walked": the count above could also be reached by asking four
    // times for the same page.
    expect(terms[1].resets).toBe(1);
  });

  it("does not page forward off the join when the last page is parsed", async () => {
    const h = harness();
    const view = await attachAndBackfill(h, 93374, 93374 - FETCH_MAX);
    await scrollLiveToTop();
    h.req.length = 0;
    await act(async () => {
      fireEvent.click(view.getByRole("button", { name: /open history/ }));
    });
    await act(async () => {
      for (let i = 0; i < WINDOW_LINES / PAGE_LINES; i++) {
        const [start, max] = h.req[h.req.length - 1];
        h.serve(start, max);
      }
    });
    // The second way xterm reaches an edge on its own: a page's lines are
    // parsed on xterm's queue, so their scroll events land AFTER the last
    // page cleared `loading` — and `open()` deliberately parks the viewport
    // at the bottom edge, because the line the user wants is the one abutting
    // the live buffer. Read as a gesture, that is `later()`: the viewer would
    // slide half a window past the join the instant it finished opening, and
    // then again, and again, until it reached the newest window — which is
    // the part the live terminal was already showing. The reported bug,
    // reproduced inside the feature that explains it.
    const anchor = 93374 - FETCH_MAX - WINDOW_LINES;
    expect(h.req.length).toBe(WINDOW_LINES / PAGE_LINES);
    expect(view.getByTestId("history-position").textContent).toBe(
      `${(anchor + 1).toLocaleString("en-US")}–${(anchor + WINDOW_LINES).toLocaleString("en-US")} of 93,374`,
    );
    // …and the user's own scroll to that same edge still pages, so this is a
    // gate on who caused the event and not on the edge itself.
    await act(async () => {
      terms[1].buf.viewportY = terms[1].buf.baseY;
      terms[1].fireScroll();
    });
    expect(h.req[h.req.length - 1]).toEqual([anchor + WINDOW_LINES / 2, PAGE_LINES]);
  });

  it("opens at the join even when stored lines are wider than the viewer's grid", async () => {
    const h = harness();
    const view = await attachAndBackfill(h, 93374, 93374 - FETCH_MAX);
    await scrollLiveToTop();
    h.req.length = 0;
    await act(async () => {
      fireEvent.click(view.getByRole("button", { name: /open history/ }));
    });
    // The session was 155 cols when this history was written and is 124 now
    // (⤢, or any window resize), so every stored line is two rows here.
    terms[1].wrap = 2;
    await act(async () => {
      for (let i = 0; i < WINDOW_LINES / PAGE_LINES; i++) {
        const [start, max] = h.req[h.req.length - 1];
        h.serve(start, max);
      }
    });
    // The last line of the window at the foot of the viewport. Counting lines
    // instead of rows gives 3,970 — half a window too high, so the join with
    // the live buffer opens off screen and the user has to go looking for it.
    const scrolls = terms[1].scrolledTo.filter(([, kind]) => kind === "line");
    expect(scrolls[scrolls.length - 1]).toEqual([WINDOW_LINES * 2 - terms[1].rows, "line"]);
  });

  it("keeps the arrow and page keys out of the session while the viewer is open", async () => {
    const h = harness();
    const view = await attachAndBackfill(h, 93374, 93374 - FETCH_MAX);
    await scrollLiveToTop();
    await act(async () => {
      fireEvent.click(view.getByRole("button", { name: /open history/ }));
    });
    const overlay = view.getByTestId("history-overlay");
    // The keys only stay out of the session because this element has focus.
    // Asserted separately because every keyDown below is dispatched AT the
    // overlay, so they would pass even with focus left on the live terminal —
    // and that failure looks like history keys typing into the session.
    expect(document.activeElement).toBe(overlay);
    for (const key of ["ArrowUp", "ArrowDown", "PageUp", "PageDown"]) {
      await act(async () => {
        fireEvent.keyDown(overlay, { key });
      });
    }
    // Handled by the viewer's own terminal — the container div cannot
    // scroll xterm's buffer natively, so these must be explicit calls.
    expect(terms[1].scrolledTo.map(([n, kind]) => `${kind}:${n}`)).toEqual(
      expect.arrayContaining(["lines:-1", "lines:1", "pages:-1", "pages:1"]),
    );
  });
});
