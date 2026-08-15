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

// A row is read by a person and clicked by a person, and those are two
// different strings: the rendered one is filtered (../displayName.ts), the
// one sent to the daemon is the exact name it stored. Codepoints appear as
// escapes — a test file carrying a raw RIGHT-TO-LEFT OVERRIDE would render
// its own source reversed.
const RLO = "\u202E"; // reorders its neighbours; deleted for display
const ZWSP = "\u200B"; // invisible; shown as U+FFFD so the row stays distinct
const MARK = "\uFFFD";

describe("Sidebar name display", () => {
  it("renders a reordering name without the override but kills the real one", async () => {
    // Measured in PR #81: this name turns the prompt into
    // "Kill proj.sdne ssecorp dlihc stI ?sh.log" — a dialog that names
    // something other than what it ends.
    const raw = "proj" + RLO + "gol.hs";
    const { api } = mockApi([row(raw, 4242)]);
    await mount(api);

    const label = screen.getByText("projgol.hs");
    expect(label.textContent).not.toContain(RLO);

    rightClick(label);
    await act(async () => {
      fireEvent.click(screen.getByRole("button", { name: "Kill" }));
    });
    // The whole point of filtering for DISPLAY only: the daemon addresses
    // sessions by their exact bytes, and a filtered name would miss.
    expect(api.killSession).toHaveBeenCalledWith(raw);
  });

  it("keeps an invisible-character decoy distinguishable from its target", async () => {
    // Both rows render "deploy" if the invisible character is stripped, and
    // then clicking either one is a coin toss over whose PTY gets the keys.
    const decoy = "de" + ZWSP + "ploy";
    const picked: string[] = [];
    const { api } = mockApi([row("deploy", 4242), row(decoy, 4243)]);
    render(<Sidebar api={api} active={null} onSelect={(n) => picked.push(n)} />);
    await act(async () => {});

    expect(screen.getByText("deploy")).toBeTruthy();
    expect(screen.getByText("de" + MARK + "ploy")).toBeTruthy();

    fireEvent.click(screen.getByText("de" + MARK + "ploy"));
    expect(picked).toEqual([decoy]);
  });

  it("names the pid in the kill confirmation", async () => {
    // The mitigation for the spoof no character rule can catch: `deploy`
    // and `dеploy` with a Cyrillic е render identically, and only a number
    // tells the reader which one is about to end.
    const { api } = mockApi([row("deploy", 4242), row("d\u0435ploy", 4243)]);
    await mount(api);

    rightClick(screen.getByText("d\u0435ploy"));
    const group = screen.getByRole("group", { name: /confirm killing/i });
    expect(group.textContent).toContain("pid 4243");
  });

  it("filters the mute button's label while muting the real name", async () => {
    const raw = "proj" + RLO + "gol.hs";
    const toggled: string[] = [];
    const { api } = mockApi([row(raw, 4242)]);
    render(
      <Sidebar
        api={api}
        active={null}
        onSelect={() => {}}
        onToggleMute={(n) => toggled.push(n)}
      />,
    );
    await act(async () => {});

    const btn = screen.getByRole("button", { name: "mute notifications for projgol.hs" });
    fireEvent.click(btn);
    expect(toggled).toEqual([raw]);
  });
});
