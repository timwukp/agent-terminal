import { describe, expect, it } from "vitest";
import {
  PanelStream,
  type PanelFrame,
  type PanelKind,
  type StreamHost,
} from "./panelStream";

interface Opened {
  kinds: PanelKind[];
  onFrame: (frame: PanelFrame) => void;
  id: number;
}

class FakeHost implements StreamHost {
  opens: Opened[] = [];
  closed: number[] = [];
  /** When set, every open rejects with it. */
  fail: string | null = null;
  private next = 1;

  open(kinds: readonly PanelKind[], onFrame: (frame: PanelFrame) => void): Promise<number> {
    const id = this.next++;
    this.opens.push({ kinds: [...kinds], onFrame, id });
    return this.fail === null ? Promise.resolve(id) : Promise.reject(this.fail);
  }
  close(id: number): void {
    this.closed.push(id);
  }
  /** The stream opened most recently. */
  last(): Opened {
    return this.opens[this.opens.length - 1];
  }
}

/** Let the open/close promise chains settle. */
const flush = () => new Promise<void>((r) => setTimeout(r, 0));

function collect() {
  const data: unknown[] = [];
  const errors: string[] = [];
  return {
    data,
    errors,
    onData: (d: unknown) => data.push(d),
    onError: (m: string) => errors.push(m),
  };
}

describe("PanelStream", () => {
  it("opens one stream naming exactly the subscribed kinds, in wire order", () => {
    const host = new FakeHost();
    const s = new PanelStream(host);
    s.subscribe("hook_log", () => {}, () => {});
    s.subscribe("usage", () => {}, () => {});
    expect(host.opens.length).toBe(2); // the second kind reopens
    // Wire order, not subscription order: the same set of panels must
    // always produce the same `kinds` argument, or an unchanged interest
    // is not recognizable as unchanged.
    expect(host.last().kinds).toEqual(["usage", "hook_log"]);
  });

  it("a second subscriber to an already-streamed kind does not restart it", () => {
    const host = new FakeHost();
    const s = new PanelStream(host);
    s.subscribe("hooks", () => {}, () => {});
    const off = s.subscribe("hooks", () => {}, () => {});
    expect(host.opens.length).toBe(1);
    // ...and dropping one of the two leaves the stream alone.
    off();
    expect(host.opens.length).toBe(1);
    expect(host.closed).toEqual([]);
  });

  it("a frame reaches that kind's subscribers and nobody else", () => {
    const host = new FakeHost();
    const s = new PanelStream(host);
    const usage = collect();
    const log = collect();
    s.subscribe<number>("usage", usage.onData, usage.onError);
    s.subscribe<number>("hook_log", log.onData, log.onError);
    host.last().onFrame({ kind: "usage", data: 41 });
    host.last().onFrame({ kind: "hook_log", data: 7 });
    expect(usage.data).toEqual([41]);
    expect(log.data).toEqual([7]);
    expect(usage.errors).toEqual([]);
  });

  it("ignores frames from a stream that has been superseded", () => {
    const host = new FakeHost();
    const s = new PanelStream(host);
    const usage = collect();
    s.subscribe<number>("usage", usage.onData, usage.onError);
    const superseded = host.last();
    s.subscribe("hooks", () => {}, () => {}); // reopens: generation bumps
    superseded.onFrame({ kind: "usage", data: 1 });
    expect(usage.data).toEqual([]);
    host.last().onFrame({ kind: "usage", data: 2 });
    expect(usage.data).toEqual([2]);
  });

  it("a frame with no kind reaches nobody instead of throwing", () => {
    const host = new FakeHost();
    const s = new PanelStream(host);
    const usage = collect();
    s.subscribe("usage", usage.onData, usage.onError);
    const push = host.last().onFrame;
    expect(() => push({} as PanelFrame)).not.toThrow();
    expect(() => push(null as unknown as PanelFrame)).not.toThrow();
    expect(() => push({ kind: "tokens" as PanelKind, data: 1 })).not.toThrow();
    expect(usage.data).toEqual([]);
  });

  it("the last unsubscribe closes the stream, naming the id it opened", async () => {
    const host = new FakeHost();
    const s = new PanelStream(host);
    const off = s.subscribe("usage", () => {}, () => {});
    off();
    await flush();
    expect(host.closed).toEqual([1]);
  });

  it("a close names its own stream, so a resubscribe is not killed by it", async () => {
    const host = new FakeHost();
    const s = new PanelStream(host);
    // StrictMode's mount → unmount → remount, which is every dev-mode
    // mount: the close for stream 1 must never take out stream 2.
    s.subscribe("usage", () => {}, () => {})();
    s.subscribe("usage", () => {}, () => {});
    await flush();
    expect(host.opens.map((o) => o.id)).toEqual([1, 2]);
    expect(host.closed).toEqual([1]);
  });

  it("a stream that cannot be opened tells every subscriber instead of going quiet", async () => {
    const host = new FakeHost();
    host.fail = "panel_stream not registered";
    const s = new PanelStream(host);
    const usage = collect();
    s.subscribe("usage", usage.onData, usage.onError);
    await flush();
    expect(usage.errors).toEqual(["panel_stream not registered"]);
    expect(usage.data).toEqual([]);
  });

  it("a host that throws instead of rejecting still reaches the failure path", async () => {
    const host: StreamHost = {
      open: () => {
        // What `new Channel()` does outside a webview.
        throw new TypeError("__TAURI_INTERNALS__ is undefined");
      },
      close: () => {},
    };
    const s = new PanelStream(host);
    const usage = collect();
    s.subscribe("usage", usage.onData, usage.onError);
    await flush();
    expect(usage.errors.length).toBe(1);
    expect(usage.errors[0]).toContain("__TAURI_INTERNALS__");
  });

  it("a failed open leaves nothing to close", async () => {
    const host = new FakeHost();
    host.fail = "nope";
    const s = new PanelStream(host);
    const off = s.subscribe("usage", () => {}, () => {});
    off();
    await flush();
    expect(host.closed).toEqual([]);
  });
});
