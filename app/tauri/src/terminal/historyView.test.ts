import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  HistoryView,
  type HistoryState,
  PAGE_LINES,
  STALL_MS,
  STEP_LINES,
  WINDOW_LINES,
} from "./historyView";

// Same shape as backfill.test.ts: one ordered journal rather than per-sink
// mocks, because the bug class here is "right calls, wrong order" (a reset
// after the lines it was supposed to precede empties the view) and a
// per-sink spy cannot see it.
function journal() {
  const calls: string[] = [];
  const requests: Array<[number, number]> = [];
  const states: HistoryState[] = [];
  const written: string[] = [];
  const sinks = {
    reset: () => calls.push("reset"),
    writeLine: (line: Uint8Array) => {
      const s = new TextDecoder().decode(line);
      written.push(s);
      calls.push(`line:${s}`);
    },
    request: (start: number, max: number) => {
      calls.push(`req:${start}+${max}`);
      requests.push([start, max]);
    },
    scrollTo: (line: number, where: string) => calls.push(`scroll:${line}:${where}`),
    onState: (s: HistoryState) => {
      states.push(s);
      calls.push(`state:${s.anchor}+${s.loaded}/${s.total}${s.loading ? ":loading" : ""}`);
    },
  };
  return { calls, requests, states, written, sinks };
}

/** Lines as the daemon would answer them: seq-labelled so a test can tell
 * WHICH lines landed, not just how many. */
const page = (startSeq: number, n: number) =>
  Array.from({ length: n }, (_, i) => new TextEncoder().encode(`s${startSeq + i}`));

/** Answer every outstanding request of one window from a log of `total`
 * lines, returning the number of pages served. */
function serveWindow(v: HistoryView, requests: Array<[number, number]>, total: number): number {
  let served = 0;
  while (v.wants()) {
    const [start, max] = requests[requests.length - 1];
    const n = Math.max(0, Math.min(max, total - start));
    v.onPage(start, page(start, n));
    served++;
  }
  return served;
}

const last = <T>(a: T[]): T => a[a.length - 1];

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe("HistoryView", () => {
  it("loads one window in PAGE_LINES pages and shows its end", () => {
    const { calls, requests, written, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    v.open(10000);
    expect(calls[0]).toBe("reset");
    expect(requests[0]).toEqual([10000, PAGE_LINES]);
    const pages = serveWindow(v, requests, 90000);
    expect(pages).toBe(WINDOW_LINES / PAGE_LINES);
    expect(requests.map(([s]) => s)).toEqual([10000, 11000, 12000, 13000]);
    expect(written.length).toBe(WINDOW_LINES);
    expect(written[0]).toBe("s10000");
    expect(last(written)).toBe(`s${10000 + WINDOW_LINES - 1}`);
    // Opened because the caller ran out of history going up, so the join is
    // at the END of this window.
    expect(last(calls.filter((c) => c.startsWith("scroll:")))).toBe(
      `scroll:${WINDOW_LINES - 1}:bottom`,
    );
  });

  it("a log shorter than one window loads all of it and asks no further", () => {
    const { requests, written, sinks } = journal();
    const v = new HistoryView(sinks, 1500);
    v.open(0);
    serveWindow(v, requests, 1500);
    expect(requests).toEqual([
      [0, PAGE_LINES],
      [1000, 500],
    ]);
    expect(written.length).toBe(1500);
    expect(last(requests)[0] + last(requests)[1]).toBe(1500);
  });

  it("never anchors past the last window", () => {
    const { requests, sinks } = journal();
    const v = new HistoryView(sinks, 1500); // shorter than a window
    v.open(9_000_000);
    expect(requests[0]).toEqual([0, PAGE_LINES]);
    const { requests: r2, sinks: s2 } = journal();
    const v2 = new HistoryView(s2, 90000);
    v2.toNewest();
    expect(r2[0]).toEqual([90000 - WINDOW_LINES, PAGE_LINES]);
  });

  it("earlier() overlaps by half a window and keeps the old top line in view", () => {
    const { calls, requests, written, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    v.open(10000);
    serveWindow(v, requests, 90000);
    written.length = 0;
    calls.length = 0;
    v.earlier();
    expect(calls[0]).toBe("reset");
    expect(requests[requests.length - 1][0]).toBe(10000 - STEP_LINES);
    serveWindow(v, requests, 90000);
    expect(written[0]).toBe(`s${10000 - STEP_LINES}`);
    // The line that was at the top (s10000) is now STEP_LINES into the
    // window, and it is put at the BOTTOM of the viewport so the reader
    // continues upward from exactly where they were.
    expect(last(calls.filter((c) => c.startsWith("scroll:")))).toBe(`scroll:${STEP_LINES}:bottom`);
  });

  it("later() overlaps the other way and puts the old last line on top", () => {
    const { calls, requests, written, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    v.open(10000);
    serveWindow(v, requests, 90000);
    calls.length = 0;
    written.length = 0;
    v.later();
    expect(requests[requests.length - 1][0]).toBe(10000 + STEP_LINES);
    serveWindow(v, requests, 90000);
    expect(written[0]).toBe(`s${10000 + STEP_LINES}`);
    // Old window ended at 10000+WINDOW-1; the next line down is
    // 10000+WINDOW, which sits WINDOW-STEP into the new window.
    expect(last(calls.filter((c) => c.startsWith("scroll:")))).toBe(
      `scroll:${WINDOW_LINES - STEP_LINES}:top`,
    );
  });

  it("earlier() at the oldest line and later() at the newest do nothing", () => {
    const { calls, requests, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    v.toOldest();
    serveWindow(v, requests, 90000);
    calls.length = 0;
    v.earlier();
    expect(calls).toEqual([]);
    v.toNewest();
    serveWindow(v, requests, 90000);
    calls.length = 0;
    v.later();
    expect(calls).toEqual([]);
  });

  it("state reports which directions exist, so the UI can disable them", () => {
    const { requests, states, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    v.toOldest();
    serveWindow(v, requests, 90000);
    expect(last(states)).toMatchObject({
      anchor: 0,
      loaded: WINDOW_LINES,
      total: 90000,
      loading: false,
      stalled: false,
      canEarlier: false,
      canLater: true,
    });
    v.toNewest();
    serveWindow(v, requests, 90000);
    expect(last(states)).toMatchObject({
      anchor: 90000 - WINDOW_LINES,
      loaded: WINDOW_LINES,
      canEarlier: true,
      canLater: false,
    });
  });

  it("ignores a page it never asked for — the live backfill's pages", () => {
    const { calls, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    // Not open yet: wants() is false, so the host routes to Backfill and
    // this machine must stay inert even if handed a page directly.
    expect(v.wants()).toBe(false);
    v.onPage(0, page(0, 10));
    expect(calls).toEqual([]);
    // And once a window is fully loaded it stops wanting, so a late page
    // from a stalled-then-answered request cannot append to it.
    const { requests, sinks: s2, calls: c2 } = journal();
    const v2 = new HistoryView(s2, 5000);
    v2.open(0);
    serveWindow(v2, requests, 5000);
    expect(v2.wants()).toBe(false);
    const before = c2.length;
    v2.onPage(WINDOW_LINES, page(WINDOW_LINES, 100));
    expect(c2.length).toBe(before);
  });

  it("follows the daemon when rotation moved the log's oldest line forward", () => {
    const { requests, states, written, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    v.open(10000);
    // Asked from 10000; the log now starts at 12345.
    v.onPage(12345, page(12345, PAGE_LINES));
    expect(written[0]).toBe("s12345");
    expect(last(states).anchor).toBe(12345);
    // The next ask continues from the answer, not from the request.
    expect(last(requests)).toEqual([12345 + PAGE_LINES, PAGE_LINES]);
  });

  it("stops on a gap rather than showing non-adjacent lines as adjacent", () => {
    const { requests, written, states, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    v.open(10000);
    v.onPage(10000, page(10000, PAGE_LINES));
    // Second page starts one line late: writing it would make the position
    // readout lie about every line after the seam.
    v.onPage(11001, page(11001, PAGE_LINES));
    expect(written.length).toBe(PAGE_LINES);
    expect(last(states).loading).toBe(false);
    expect(requests.length).toBe(2); // no third ask
  });

  it("an empty page ends the window (the log no longer goes that deep)", () => {
    const { requests, written, states, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    v.open(10000);
    v.onPage(10000, []);
    expect(written).toEqual([]);
    expect(requests.length).toBe(1);
    expect(last(states)).toMatchObject({ loading: false, loaded: 0 });
  });

  it("a stalled page is reported, and retry continues the same window", () => {
    const { calls, requests, states, written, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    v.open(10000);
    v.onPage(10000, page(10000, PAGE_LINES));
    vi.advanceTimersByTime(STALL_MS);
    expect(last(states)).toMatchObject({ loading: false, stalled: true, loaded: PAGE_LINES });
    calls.length = 0;
    v.retry();
    // Continues from where it stopped: the 1000 lines on screen are correct
    // and re-fetching them would be a worse answer than a partial window.
    expect(last(requests)).toEqual([10000 + PAGE_LINES, PAGE_LINES]);
    expect(calls).not.toContain("reset");
    // …and the retry is visible the instant it is made, not only when the page
    // lands. The overlay renders its red "stalled — retry" button off these
    // two flags, so an un-emitted retry leaves that button up for the whole
    // 3 s a request is in flight while a second click on it does nothing —
    // the control reads as broken precisely when it is working.
    expect(last(states)).toMatchObject({ loading: true, stalled: false });
    serveWindow(v, requests, 90000);
    expect(written.length).toBe(WINDOW_LINES);
    expect(last(states).stalled).toBe(false);
  });

  it("a response resets the stall clock instead of inheriting it", () => {
    const { states, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    v.open(10000);
    vi.advanceTimersByTime(STALL_MS - 1);
    v.onPage(10000, page(10000, PAGE_LINES));
    vi.advanceTimersByTime(STALL_MS - 1);
    expect(last(states).stalled).toBe(false);
    vi.advanceTimersByTime(1);
    expect(last(states).stalled).toBe(true);
  });

  it("setTotal raises the ceiling without moving the window", () => {
    const { requests, states, sinks } = journal();
    const v = new HistoryView(sinks, 5000);
    v.open(0);
    serveWindow(v, requests, 5000);
    const anchor = last(states).anchor;
    v.setTotal(9000);
    expect(last(states)).toMatchObject({ anchor, total: 9000, canLater: true });
    v.setTotal(100); // a stale snapshot must not shrink the log
    expect(last(states).total).toBe(9000);
  });

  it("a session with no history opens nothing at all", () => {
    const { calls, sinks } = journal();
    const v = new HistoryView(sinks, 0);
    v.open(0);
    expect(calls).toEqual([]);
  });

  it("dispose cancels the stall timer and silences every later event", () => {
    const { calls, requests, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    v.open(10000);
    const before = calls.length;
    v.dispose();
    vi.advanceTimersByTime(STALL_MS * 2);
    v.onPage(10000, page(10000, PAGE_LINES));
    v.earlier();
    v.setTotal(99999);
    expect(calls.length).toBe(before);
    expect(requests.length).toBe(1);
  });

  it("a move while a page is outstanding is ignored, not queued", () => {
    const { requests, sinks } = journal();
    const v = new HistoryView(sinks, 90000);
    v.open(10000);
    expect(v.wants()).toBe(true);
    v.earlier();
    v.later();
    v.toOldest();
    v.toNewest();
    expect(requests.length).toBe(1);
  });
});
