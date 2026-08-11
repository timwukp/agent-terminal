import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { Backfill, FETCH_MAX, PAGE_LINES, STALL_MS } from "./backfill";

// The machine's contract is an ORDER of sink calls; every test asserts
// against this single journal rather than per-sink mocks, because "the
// right calls, wrong order" is exactly the bug class this module exists
// to prevent.
function journal() {
  const calls: string[] = [];
  const requests: Array<[number, number]> = [];
  const sinks = {
    writeLine: (line: Uint8Array) => calls.push(`line:${new TextDecoder().decode(line)}`),
    padToScrollback: () => calls.push("pad"),
    applySnapshot: (cols: number, rows: number, blob: Uint8Array) =>
      calls.push(`snap:${cols}x${rows}:${new TextDecoder().decode(blob)}`),
    writeOutput: (bytes: Uint8Array) => calls.push(`out:${new TextDecoder().decode(bytes)}`),
    request: (start: number, max: number) => {
      calls.push(`req:${start}+${max}`);
      requests.push([start, max]);
    },
  };
  return { calls, requests, sinks };
}

const enc = (s: string) => new TextEncoder().encode(s);
const lines = (n: number, prefix = "l") =>
  Array.from({ length: n }, (_, i) => enc(`${prefix}${i}`));

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe("Backfill", () => {
  it("orders history before the repaint and queued output after it", () => {
    const { calls, sinks } = journal();
    const b = new Backfill(sinks);
    b.onOutput(enc("early")); // arrives before the snapshot: still queued
    b.onSnapshot(120, 30, 2, enc("PAINT"));
    b.onOutput(enc("mid")); // arrives during the fetch
    b.onScrollback(0, [enc("h0"), enc("h1")]);
    b.onOutput(enc("late")); // after finish: passthrough
    expect(calls).toEqual([
      "req:0+2",
      "line:h0",
      "line:h1",
      "pad",
      "snap:120x30:PAINT",
      "out:early",
      "out:mid",
      "out:late",
    ]);
  });

  it("a session with no history paints immediately, no request, no pad", () => {
    const { calls, sinks } = journal();
    const b = new Backfill(sinks);
    b.onSnapshot(80, 24, 0, enc("P"));
    expect(calls).toEqual(["snap:80x24:P"]);
  });

  it("pages sequentially and stops exactly at the announced total", () => {
    const { requests, calls, sinks } = journal();
    const b = new Backfill(sinks);
    b.onSnapshot(80, 24, 2500, enc("P"));
    expect(requests).toEqual([[0, PAGE_LINES]]);
    b.onScrollback(0, lines(1000));
    expect(requests[1]).toEqual([1000, PAGE_LINES]);
    b.onScrollback(1000, lines(1000));
    // Only 500 remain: never ask past the total the snapshot announced.
    expect(requests[2]).toEqual([2000, 500]);
    b.onScrollback(2000, lines(500));
    expect(calls.filter((c) => c.startsWith("line:")).length).toBe(2500);
    expect(calls[calls.length - 2]).toBe("pad");
    expect(calls[calls.length - 1]).toBe("snap:80x24:P");
  });

  it("starts the fetch at total-FETCH_MAX when history exceeds the ring", () => {
    const { requests, sinks } = journal();
    const b = new Backfill(sinks);
    b.onSnapshot(80, 24, FETCH_MAX + 5000, enc("P"));
    expect(requests).toEqual([[5000, PAGE_LINES]]);
  });

  it("follows the daemon when eviction moved the start forward", () => {
    const { requests, sinks } = journal();
    const b = new Backfill(sinks);
    b.onSnapshot(80, 24, 2000, enc("P"));
    // Asked from 0, but the ring only starts at 800 now.
    b.onScrollback(800, lines(1000));
    // Next page continues from where the answer ended, not the ask.
    expect(requests[1]).toEqual([1800, 200]);
  });

  it("clamps lines past the announced total (they are already in queued output)", () => {
    const { calls, sinks } = journal();
    const b = new Backfill(sinks);
    b.onSnapshot(80, 24, 3, enc("P"));
    // The ring grew while we fetched: 5 lines come back, only 3 predate
    // the snapshot. Writing l3/l4 would show them twice.
    b.onScrollback(0, lines(5));
    expect(calls.filter((c) => c.startsWith("line:"))).toEqual(["line:l0", "line:l1", "line:l2"]);
    expect(calls[calls.length - 1]).toMatch(/^snap:/);
  });

  it("an empty page ends the fetch (ring gone, e.g. daemon restarted)", () => {
    const { calls, sinks } = journal();
    const b = new Backfill(sinks);
    b.onSnapshot(80, 24, 500, enc("P"));
    b.onScrollback(0, []);
    // No lines were written, so no pad — a pad here would scroll the
    // blank mount screen into scrollback as phantom empty history.
    expect(calls).toEqual(["req:0+500", "snap:80x24:P"]);
  });

  it("a stalled fetch paints without history, and a late page is dropped", () => {
    const { calls, sinks } = journal();
    const b = new Backfill(sinks);
    b.onSnapshot(80, 24, 100, enc("P"));
    b.onOutput(enc("queued"));
    vi.advanceTimersByTime(STALL_MS);
    expect(calls).toEqual(["req:0+100", "snap:80x24:P", "out:queued"]);
    // The page finally shows up: history under the live screen would
    // corrupt it, so nothing may happen.
    b.onScrollback(0, lines(100));
    expect(calls.length).toBe(3);
  });

  it("a response resets the stall clock instead of inheriting it", () => {
    const { calls, sinks } = journal();
    const b = new Backfill(sinks);
    b.onSnapshot(80, 24, 2000, enc("P"));
    vi.advanceTimersByTime(STALL_MS - 1);
    b.onScrollback(0, lines(1000)); // page 1 landed just in time
    vi.advanceTimersByTime(STALL_MS - 1);
    // Not stalled: the second page's clock started at the first's reply.
    expect(calls.some((c) => c.startsWith("snap:"))).toBe(false);
    vi.advanceTimersByTime(1);
    expect(calls.some((c) => c.startsWith("snap:"))).toBe(true);
  });

  it("a mid-fetch snapshot replaces the stash; the newest screen paints once", () => {
    const { calls, sinks } = journal();
    const b = new Backfill(sinks);
    b.onSnapshot(80, 24, 1, enc("OLD"));
    b.onSnapshot(90, 30, 1, enc("NEW")); // geometry changed during fetch
    b.onScrollback(0, [enc("h")]);
    expect(calls.filter((c) => c.startsWith("snap:"))).toEqual(["snap:90x30:NEW"]);
  });

  it("post-attach snapshots pass straight through with no history fetch", () => {
    const { calls, sinks } = journal();
    const b = new Backfill(sinks);
    b.onSnapshot(80, 24, 0, enc("P1"));
    b.onSnapshot(100, 40, 7, enc("P2")); // sb_lines present but irrelevant now
    expect(calls).toEqual(["snap:80x24:P1", "snap:100x40:P2"]);
  });

  it("dispose cancels the stall timer and silences every later event", () => {
    const { calls, sinks } = journal();
    const b = new Backfill(sinks);
    b.onSnapshot(80, 24, 100, enc("P"));
    b.dispose();
    vi.advanceTimersByTime(STALL_MS);
    b.onScrollback(0, lines(100));
    expect(calls).toEqual(["req:0+100"]);
  });
});
