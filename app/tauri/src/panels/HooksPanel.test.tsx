// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { act, cleanup, fireEvent, render } from "@testing-library/react";
import HooksPanel from "./HooksPanel";
import {
  commandBasename,
  isScriptPath,
  type HooksApi,
  type HooksSnapshot,
} from "./hooksApi";

function snapshot(over: Partial<HooksSnapshot> = {}): HooksSnapshot {
  return {
    path: "/home/u/.claude/settings.json",
    exists: true,
    rules: [
      {
        event: "PreToolUse",
        matcher: "Bash",
        command: "/home/u/.claude/hooks/block-git-push.sh",
        timeout: null,
      },
      {
        event: "PreToolUse",
        matcher: "Bash",
        command: "/home/u/.claude/hooks/check-redaction.sh",
        timeout: null,
      },
      { event: "Stop", matcher: "*", command: "notify send-done", timeout: 30 },
    ],
    malformed: 0,
    ...over,
  };
}

function apiOf(snap: HooksSnapshot, script = "#!/bin/sh\n# Blocks: git push\nexit 2\n") {
  return {
    calls: 0,
    scriptCalls: [] as string[],
    snapshot() {
      this.calls++;
      return Promise.resolve(snap);
    },
    readScript(command: string) {
      this.scriptCalls.push(command);
      return Promise.resolve(script);
    },
  } satisfies HooksApi & { calls: number; scriptCalls: string[] };
}

beforeEach(() => vi.useFakeTimers());
afterEach(() => {
  cleanup();
  vi.useRealTimers();
});

describe("HooksPanel", () => {
  it("groups rows by event, one row per command, with matcher and timeout", async () => {
    const view = render(<HooksPanel api={apiOf(snapshot())} />);
    await act(async () => {});
    expect(view.container.textContent).toContain("PreToolUse");
    expect(view.container.textContent).toContain("block-git-push.sh");
    expect(view.container.textContent).toContain("check-redaction.sh");
    expect(view.container.textContent).toContain("Stop");
    expect(view.container.textContent).toContain("30s");
  });

  it("clicking a script row fetches and shows its source, read-only", async () => {
    const api = apiOf(snapshot());
    const view = render(<HooksPanel api={api} />);
    await act(async () => {});
    fireEvent.click(view.getByTitle(/block-git-push\.sh — click/));
    await act(async () => {});
    expect(api.scriptCalls).toEqual(["/home/u/.claude/hooks/block-git-push.sh"]);
    expect(view.container.textContent).toContain("# Blocks: git push");
  });

  it("an inline command shows the command itself without a file read", async () => {
    const api = apiOf(snapshot());
    const view = render(<HooksPanel api={api} />);
    await act(async () => {});
    fireEvent.click(view.getByTitle(/notify send-done — click/));
    await act(async () => {});
    // No readScript round trip: the command string IS the source.
    expect(api.scriptCalls).toEqual([]);
    expect(view.container.querySelector("pre")?.textContent).toBe("notify send-done");
  });

  it("shows honest empty states and the unparsed badge", async () => {
    const none = render(
      <HooksPanel api={apiOf(snapshot({ exists: false, rules: [], malformed: 0 }))} />,
    );
    await act(async () => {});
    expect(none.container.textContent).toContain("no /home/u/.claude/settings.json found");
    cleanup();

    const empty = render(<HooksPanel api={apiOf(snapshot({ rules: [], malformed: 0 }))} />);
    await act(async () => {});
    expect(empty.container.textContent).toContain("no hooks configured");
    cleanup();

    const bad = render(<HooksPanel api={apiOf(snapshot({ malformed: 2 }))} />);
    await act(async () => {});
    expect(bad.container.textContent).toContain("2 unparsed");
  });

  it("polls while mounted and stops after unmount", async () => {
    const api = apiOf(snapshot());
    const view = render(<HooksPanel api={api} />);
    await act(async () => {});
    expect(api.calls).toBe(1);
    await act(async () => {
      vi.advanceTimersByTime(2000);
    });
    expect(api.calls).toBe(2);
    view.unmount();
    await act(async () => {
      vi.advanceTimersByTime(6000);
    });
    expect(api.calls).toBe(2);
  });
});

describe("hooksApi helpers", () => {
  it("commandBasename takes the last path segment and keeps inline commands whole", () => {
    expect(commandBasename("/a/b/guard.sh")).toBe("guard.sh");
    expect(commandBasename("notify send-done")).toBe("notify send-done");
  });
  it("isScriptPath is true only for bare absolute paths", () => {
    expect(isScriptPath("/a/b/guard.sh")).toBe(true);
    expect(isScriptPath("python3 -c 'x'")).toBe(false);
    expect(isScriptPath("/a/b/guard.sh --flag")).toBe(false);
  });
});
