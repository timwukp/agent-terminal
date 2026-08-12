// @vitest-environment jsdom
//
// Kill is the only destructive thing this GUI does, and it was guarded by
// `window.confirm` — which on macOS in wry 0.55.1 completes FALSE without
// asking, because that WKWebView UI delegate implements no
// runJavaScriptConfirmPanel. The button therefore did nothing at all in
// every packaged build, and no test could see it: jsdom's `confirm` is also
// a stub, so a suite that mocked it would have proved the opposite of what
// shipped. These tests assert on rendered DOM instead, which is the only
// thing both platforms agree on.

import { describe, expect, it, vi, beforeEach, afterEach } from "vitest";
import { render, fireEvent, screen, cleanup, act } from "@testing-library/react";
import Sidebar from "./Sidebar";
import type { ControlApi, SessionRow, Template } from "./api";

const row = (name: string, pid: number): SessionRow => ({
  name,
  view_cols: 80,
  view_rows: 24,
  alive: true,
  nclients: 0,
  pid,
  npanes: 1,
  zoomed: false,
});

function mockApi(rows: SessionRow[], templates: Template[] = []) {
  const state = { rows };
  const api: ControlApi = {
    listSessions: vi.fn(async () => state.rows),
    newSession: vi.fn(async () => {}),
    killSession: vi.fn(async () => {}),
    listTemplates: vi.fn(async () => templates),
  };
  return { api, state };
}

/** Render and let the first poll resolve, so rows exist to click. */
async function mount(api: ControlApi) {
  const r = render(<Sidebar api={api} active={null} onSelect={() => {}} />);
  await act(async () => {});
  return r;
}

const rightClick = (el: Element) => fireEvent.contextMenu(el);

beforeEach(() => vi.useFakeTimers({ shouldAdvanceTime: true }));
afterEach(() => {
  cleanup();
  vi.useRealTimers();
});

describe("Sidebar kill confirmation", () => {
  it("asks in the DOM, not through window.confirm", async () => {
    // The mutation-proof form of "it asks": window.confirm is replaced with
    // a spy that would ACCEPT. If the component still called it, the kill
    // would go through with no prompt rendered, and both assertions here
    // would fail — where a spy returning false could be satisfied by a
    // component that simply does nothing.
    const confirmSpy = vi.fn(() => true);
    vi.stubGlobal("confirm", confirmSpy);
    const { api } = mockApi([row("work", 4242)]);
    await mount(api);

    rightClick(screen.getByText("work"));

    expect(confirmSpy).not.toHaveBeenCalled();
    expect(api.killSession).not.toHaveBeenCalled();
    expect(screen.getByRole("group", { name: /confirm killing session work/i })).toBeTruthy();
    vi.unstubAllGlobals();
  });

  it("kills only after the confirm button, and names the session it asked about", async () => {
    const { api } = mockApi([row("work", 4242), row("logs", 4243)]);
    await mount(api);

    rightClick(screen.getByText("logs"));
    const group = screen.getByRole("group", { name: /confirm killing session logs/i });
    expect(group.textContent).toContain("logs");
    expect(api.killSession).not.toHaveBeenCalled();

    await act(async () => {
      fireEvent.click(screen.getByRole("button", { name: "Kill" }));
    });
    expect(api.killSession).toHaveBeenCalledWith("logs");
    expect(api.killSession).toHaveBeenCalledTimes(1);
    // The prompt is gone afterwards, and it took the row with it.
    expect(screen.queryByRole("group", { name: /confirm killing/i })).toBeNull();
  });

  it("cancel and Escape both dismiss without killing", async () => {
    const { api } = mockApi([row("work", 4242)]);
    await mount(api);

    rightClick(screen.getByText("work"));
    fireEvent.click(screen.getByRole("button", { name: "Cancel" }));
    expect(api.killSession).not.toHaveBeenCalled();
    expect(screen.queryByRole("group", { name: /confirm killing/i })).toBeNull();
    // The row comes back, so cancelling is not a way to lose a session.
    expect(screen.getByText("work")).toBeTruthy();

    rightClick(screen.getByText("work"));
    fireEvent.keyDown(screen.getByRole("group", { name: /confirm killing session work/i }), {
      key: "Escape",
    });
    expect(api.killSession).not.toHaveBeenCalled();
    expect(screen.queryByRole("group", { name: /confirm killing/i })).toBeNull();
  });

  it("focuses Cancel, not Kill", async () => {
    // A destructive prompt must not be confirmable by the Return someone is
    // already pressing — the sidebar rows are buttons, so a keyboard user
    // arrives here mid-keystroke.
    const { api } = mockApi([row("work", 4242)]);
    await mount(api);

    rightClick(screen.getByText("work"));
    expect(document.activeElement).toBe(screen.getByRole("button", { name: "Cancel" }));
  });

  it("drops the prompt when its session dies underneath it", async () => {
    const { api, state } = mockApi([row("work", 4242)]);
    await mount(api);
    rightClick(screen.getByText("work"));

    state.rows = [];
    await act(async () => {
      vi.advanceTimersByTime(2000);
    });

    expect(screen.queryByRole("group", { name: /confirm killing/i })).toBeNull();
    expect(api.killSession).not.toHaveBeenCalled();
  });

  it("drops the prompt when the name comes back on a different pid", async () => {
    // The daemon addresses sessions by NAME, and nextSessionName reuses a
    // freed one, so a prompt left standing across that gap would kill a
    // session its reader never saw. The pid is what distinguishes them.
    const { api, state } = mockApi([row("claude", 4242)]);
    await mount(api);
    rightClick(screen.getByText("claude"));

    state.rows = [row("claude", 5555)];
    await act(async () => {
      vi.advanceTimersByTime(2000);
    });

    expect(screen.queryByRole("group", { name: /confirm killing/i })).toBeNull();
    // Discrimination control: the same poll with the SAME pid keeps it up,
    // so the guard above is about identity and not about polling at all.
    rightClick(screen.getByText("claude"));
    state.rows = [row("claude", 5555)];
    await act(async () => {
      vi.advanceTimersByTime(2000);
    });
    expect(screen.getByRole("group", { name: /confirm killing session claude/i })).toBeTruthy();
  });

  it("asks about one session at a time", async () => {
    const { api } = mockApi([row("work", 4242), row("logs", 4243)]);
    await mount(api);

    rightClick(screen.getByText("work"));
    rightClick(screen.getByText("logs"));

    expect(screen.getAllByRole("group", { name: /confirm killing/i }).length).toBe(1);
    expect(screen.getByRole("group", { name: /confirm killing session logs/i })).toBeTruthy();
  });
});
