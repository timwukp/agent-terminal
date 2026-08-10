import { describe, expect, it, vi } from "vitest";
import { routeMessage, type SessionEvents } from "./transport";

function events() {
  return {
    onOutput: vi.fn(),
    onSnapshot: vi.fn(),
    onCtrl: vi.fn(),
  } satisfies SessionEvents;
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
    // cols=160 (0xa0,0x00), rows=40 (0x28,0x00), blob "hi"
    routeMessage(new Uint8Array([0x31, 0xa0, 0x00, 0x28, 0x00, 104, 105]), e);
    expect(e.onSnapshot).toHaveBeenCalledWith(160, 40, new Uint8Array([104, 105]));
  });

  it("snapshot with u16 values above 255 decodes both bytes", () => {
    const e = events();
    // cols=300 = 0x012c → [0x2c, 0x01]; a single-byte reading gives 44.
    routeMessage(new Uint8Array([0x31, 0x2c, 0x01, 0x28, 0x00]), e);
    expect(e.onSnapshot).toHaveBeenCalledWith(300, 40, new Uint8Array([]));
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
    routeMessage(new Uint8Array([0x31, 0xa0]), e); // truncated header
    routeMessage(new Uint8Array([]), e);
    expect(e.onOutput).not.toHaveBeenCalled();
    expect(e.onSnapshot).not.toHaveBeenCalled();
    expect(e.onCtrl).not.toHaveBeenCalled();
  });
});
