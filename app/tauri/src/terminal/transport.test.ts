import { describe, expect, it, vi } from "vitest";
import { routeMessage, type SessionEvents } from "./transport";

function events() {
  return {
    onOutput: vi.fn(),
    onSnapshot: vi.fn(),
    onScrollback: vi.fn(),
    onCtrl: vi.fn(),
  } satisfies SessionEvents;
}

/** [tag, cols u16, rows u16, sb_lines u64 LE, blob…] */
function snapshotFrame(cols: number, rows: number, sbLines: number, blob: number[]): Uint8Array {
  const b = new Uint8Array(13 + blob.length);
  b[0] = 0x31;
  b[1] = cols & 0xff;
  b[2] = cols >> 8;
  b[3] = rows & 0xff;
  b[4] = rows >> 8;
  new DataView(b.buffer).setBigUint64(5, BigInt(sbLines), true);
  b.set(blob, 13);
  return b;
}

describe("routeMessage", () => {
  it("routes 0x30 output bytes verbatim", () => {
    const e = events();
    routeMessage(new Uint8Array([0x30, 0x1b, 0x5b, 0x32, 0x4a]), e);
    expect(e.onOutput).toHaveBeenCalledWith(new Uint8Array([0x1b, 0x5b, 0x32, 0x4a]));
    expect(e.onSnapshot).not.toHaveBeenCalled();
  });

  it("parses the snapshot header LE and passes the blob", () => {
    const e = events();
    routeMessage(snapshotFrame(160, 40, 12345, [104, 105]), e);
    expect(e.onSnapshot).toHaveBeenCalledWith(160, 40, 12345, new Uint8Array([104, 105]));
  });

  it("snapshot with u16 values above 255 decodes both bytes", () => {
    const e = events();
    // cols=300 = 0x012c; a single-byte reading gives 44.
    routeMessage(snapshotFrame(300, 40, 0, []), e);
    expect(e.onSnapshot).toHaveBeenCalledWith(300, 40, 0, new Uint8Array([]));
  });

  it("snapshot sb_lines above 2^32 keeps its high word", () => {
    const e = events();
    // 5 * 2^32 + 7: a u32 reading (or dropped high word) gives 7.
    routeMessage(snapshotFrame(80, 24, 5 * 2 ** 32 + 7, []), e);
    expect(e.onSnapshot).toHaveBeenCalledWith(80, 24, 5 * 2 ** 32 + 7, new Uint8Array([]));
  });

  it("parses an 0x33 history page into per-line views", () => {
    const e = events();
    // first_seq=9, nlines=2: "hi" (len 2), "" (len 0 — empty lines count)
    const b = new Uint8Array([
      0x33, 9, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 104, 105, 0, 0, 0, 0,
    ]);
    routeMessage(b, e);
    expect(e.onScrollback).toHaveBeenCalledWith(9, [new Uint8Array([104, 105]), new Uint8Array([])]);
  });

  it("drops a truncated 0x33 page whole rather than delivering half", () => {
    const e = events();
    // nlines says 1 but the line body is cut short.
    const b = new Uint8Array([0x33, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 104]);
    routeMessage(b, e);
    // Half a page would advance the backfill cursor past lines never
    // shown; no page at all lets the stall timer repaint instead.
    expect(e.onScrollback).not.toHaveBeenCalled();
  });

  it("parses 0x4a control JSON", () => {
    const e = events();
    const ev = { kind: "session_exited", exit_status: 7 };
    routeMessage(new Uint8Array([0x4a, ...new TextEncoder().encode(JSON.stringify(ev))]), e);
    expect(e.onCtrl).toHaveBeenCalledWith(ev);
  });

  it("skips unknown tags and malformed snapshots (wire skip rule)", () => {
    const e = events();
    routeMessage(new Uint8Array([0x99, 1, 2, 3]), e);
    routeMessage(new Uint8Array([0x31, 0xa0, 0x00, 0x28, 0x00]), e); // header now needs sb_lines too
    routeMessage(new Uint8Array([0x33, 1, 2, 3]), e); // truncated history header
    routeMessage(new Uint8Array([]), e);
    expect(e.onOutput).not.toHaveBeenCalled();
    expect(e.onSnapshot).not.toHaveBeenCalled();
    expect(e.onScrollback).not.toHaveBeenCalled();
    expect(e.onCtrl).not.toHaveBeenCalled();
  });
});
