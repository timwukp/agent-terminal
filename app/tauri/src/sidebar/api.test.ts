import { beforeEach, describe, expect, it, vi } from "vitest";

// The real module reaches into window.__TAURI_INTERNALS__ the moment a
// Channel is constructed, so it is replaced wholesale — the thing under
// test here is the frame routing and the subscribe/stop lifecycle, which
// is code that exists nowhere else.
vi.mock("@tauri-apps/api/core", () => {
  class Channel<T> {
    onmessage: ((m: T) => void) | null = null;
  }
  return { Channel, invoke: vi.fn() };
});

import { invoke } from "@tauri-apps/api/core";
import { nextSessionName, TauriControlApi, type SessionWatch } from "./api";

describe("nextSessionName", () => {
  it("bare prefix when free", () => {
    expect(nextSessionName("claude", [])).toBe("claude");
    expect(nextSessionName("claude", ["shell", "work"])).toBe("claude");
  });
  it("numbers from 2 when taken", () => {
    expect(nextSessionName("claude", ["claude"])).toBe("claude-2");
    expect(nextSessionName("claude", ["claude", "claude-2"])).toBe("claude-3");
  });
  it("fills gaps rather than counting past them", () => {
    // claude-2 died and vanished from ls; its name is free again.
    expect(nextSessionName("claude", ["claude", "claude-3"])).toBe("claude-2");
  });
});

describe("TauriControlApi.watchSessions", () => {
  /** Collected callbacks, plus the channel the command was handed. */
  function subscribe(startId: Promise<number>) {
    const pushes: boolean[] = [];
    let changed = 0;
    const watch: SessionWatch = {
      onPush: (live) => pushes.push(live),
      onChanged: () => changed++,
    };
    vi.mocked(invoke).mockImplementation((cmd) =>
      cmd === "session_watch" ? startId : Promise.resolve(undefined),
    );
    const stop = new TauriControlApi().watchSessions(watch);
    const call = vi.mocked(invoke).mock.calls.find((c) => c[0] === "session_watch");
    const chan = (call?.[1] as { chan: { onmessage: ((m: unknown) => void) | null } }).chan;
    return { pushes, changed: () => changed, chan, stop };
  }

  const flush = () => new Promise<void>((r) => setTimeout(r, 0));

  beforeEach(() => vi.mocked(invoke).mockReset());

  it("routes the two frame shapes and ignores anything else", async () => {
    const { pushes, changed, chan } = subscribe(Promise.resolve(7));
    await flush();

    chan.onmessage?.({ kind: "sessions_push", data: { live: true } });
    chan.onmessage?.({ kind: "sessions_changed" });
    chan.onmessage?.({ kind: "sessions_changed" });
    chan.onmessage?.({ kind: "sessions_push", data: { live: false } });
    // A frame from a newer backend, and two malformed ones: unknown kinds
    // are skipped exactly as the wire protocol skips unknown types, and
    // neither of these may throw inside the channel callback.
    chan.onmessage?.({ kind: "sessions_reordered", data: {} });
    chan.onmessage?.(null);
    chan.onmessage?.("sessions_changed");

    expect(pushes).toEqual([true, false]);
    expect(changed()).toBe(2);
  });

  it("reports no push when the command itself fails", async () => {
    // Silence would be read as "push is live and nothing changed", which
    // is the one interpretation that leaves the sidebar neither pushed to
    // nor polling.
    const { pushes } = subscribe(Promise.reject(new Error("command not found")));
    await flush();
    expect(pushes).toEqual([false]);
  });

  it("stops the watcher it started, by id", async () => {
    const { chan, stop, changed } = subscribe(Promise.resolve(11));
    await flush();
    stop();
    await flush();
    expect(vi.mocked(invoke).mock.calls).toContainEqual(["session_watch_stop", { id: 11 }]);
    // A frame already in flight when the stop was issued must not reach a
    // caller that has unsubscribed — in the sidebar that is a setState on
    // an unmounted component.
    chan.onmessage?.({ kind: "sessions_changed" });
    expect(changed()).toBe(0);
  });

  it("has nothing to stop when the start failed", async () => {
    const { stop } = subscribe(Promise.reject(new Error("no daemon")));
    await flush();
    stop();
    await flush();
    expect(vi.mocked(invoke).mock.calls.some((c) => c[0] === "session_watch_stop")).toBe(false);
  });
});
