// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { act, cleanup, render } from "@testing-library/react";
import SecurityCard from "./SecurityCard";
import type { HookLogSnapshot, HooksApi } from "./hooksApi";

function log(over: Partial<HookLogSnapshot> = {}): HookLogSnapshot {
  return {
    path: "/home/u/.claude/hooks/hooks.log",
    exists: true,
    events: [
      {
        ts: "2026-08-11T12:00:00Z",
        hook: "block-git-push.sh",
        event: "PreToolUse",
        tool: "Bash",
        decision: "block",
        reason: "git push is PR-only",
      },
      {
        ts: "2026-08-11T12:00:05Z",
        hook: "check-redaction.sh",
        event: "PreToolUse",
        tool: "Bash",
        decision: "allow",
        reason: "clean",
      },
    ],
    total: 2,
    malformed: 0,
    chain_ok: true,
    break_at: null,
    ...over,
  };
}

type Pushers = { calls: number; subs: ((s: HookLogSnapshot) => void)[] };

function apiOf(snap: HookLogSnapshot): HooksApi & Pushers {
  const api: HooksApi & Pushers = {
    calls: 0,
    subs: [],
    snapshot: () =>
      Promise.resolve({ path: "", exists: false, rules: [], malformed: 0 }),
    readScript: () => Promise.reject(new Error("unused")),
    subscribe: () => () => {},
    logSnapshot() {
      api.calls++;
      return Promise.resolve(snap);
    },
    subscribeLog(onData) {
      api.subs.push(onData);
      return () => {
        api.subs = api.subs.filter((f) => f !== onData);
      };
    },
  };
  return api;
}

beforeEach(() => vi.useFakeTimers());
afterEach(() => {
  cleanup();
  vi.useRealTimers();
});

describe("SecurityCard", () => {
  it("verified chain: green badge with the event count, events newest first", async () => {
    const view = render(<SecurityCard api={apiOf(log())} />);
    await act(async () => {});
    expect(view.container.textContent).toContain("chain verified · 2 events");
    const items = view.container.querySelectorAll("li");
    expect(items.length).toBe(2);
    // Newest (12:00:05, allow) renders first.
    expect(items[0].textContent).toContain("allow");
    expect(items[1].textContent).toContain("block");
  });

  it("broken chain: names the 1-based line, keeps showing history", async () => {
    const view = render(
      <SecurityCard api={apiOf(log({ chain_ok: false, break_at: 1 }))} />,
    );
    await act(async () => {});
    expect(view.container.textContent).toContain("chain broken at line 2");
    expect(view.container.querySelectorAll("li").length).toBe(2);
  });

  it("no log file: honest empty state pointing at the design doc", async () => {
    const view = render(
      <SecurityCard api={apiOf(log({ exists: false, events: [], total: 0 }))} />,
    );
    await act(async () => {});
    expect(view.container.textContent).toContain("no hook log");
    expect(view.container.textContent).toContain("hook-log.md");
  });

  it("unparsed lines ride the badge", async () => {
    const view = render(<SecurityCard api={apiOf(log({ malformed: 3 }))} />);
    await act(async () => {});
    expect(view.container.textContent).toContain("3 unparsed");
  });

  it("reads once, repaints from pushed frames, and unsubscribes on unmount", async () => {
    const api = apiOf(log());
    const view = render(<SecurityCard api={api} />);
    await act(async () => {});
    expect(api.calls).toBe(1);
    expect(api.subs.length).toBe(1);
    await act(async () => {
      api.subs[0](log({ chain_ok: false, break_at: 1 }));
    });
    expect(view.container.textContent).toContain("chain broken at line 2");
    expect(api.calls).toBe(1); // pushed, not re-fetched
    view.unmount();
    expect(api.subs.length).toBe(0);
  });
});
