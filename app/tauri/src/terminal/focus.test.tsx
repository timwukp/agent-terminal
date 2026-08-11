// @vitest-environment jsdom
//
// Focus is the difference between a working terminal and one the user
// describes as "I typed and nothing happened". It cannot be covered by the
// logic-only suites: it lives entirely in DOM state, and both times this
// GUI lost input in UAT the keystrokes were going to something that was
// not the terminal.
//
// xterm.js is stubbed rather than rendered: a real XTerm needs canvas
// measurement jsdom does not provide, and the assertions here are about
// which element holds focus and when — not about rendering.

import { describe, expect, it, vi, beforeEach, afterEach } from "vitest";
import { render, fireEvent, act, cleanup } from "@testing-library/react";
import App from "../App";

const focusSpy = vi.fn();

vi.mock("@xterm/xterm", () => {
  class FakeTerm {
    element: HTMLElement | null = null;
    cols = 80;
    rows = 24;
    open(host: HTMLElement) {
      // Mimic the real thing closely enough for focus to be meaningful:
      // xterm renders a focusable textarea and focuses *that*.
      const el = document.createElement("div");
      const ta = document.createElement("textarea");
      el.appendChild(ta);
      host.appendChild(el);
      this.element = el;
    }
    focus() {
      focusSpy();
      this.element?.querySelector("textarea")?.focus();
    }
    write() {}
    resize() {}
    scrollToBottom() {}
    dispose() {}
    onData() {
      return { dispose() {} };
    }
  }
  return { Terminal: FakeTerm };
});
vi.mock("@xterm/xterm/css/xterm.css", () => ({}));

// One session, so the sidebar renders exactly one row to click. Sidebar
// also imports nextSessionName from here, so the mock must keep it.
vi.mock("../sidebar/api.ts", () => ({
  TauriControlApi: class {
    listSessions() {
      return Promise.resolve([
        { name: "work", view_cols: 80, view_rows: 24, pid: 1, nclients: 1 },
      ]);
    }
    listTemplates() {
      return Promise.resolve([]);
    }
    newSession() {
      return Promise.resolve();
    }
    killSession() {
      return Promise.resolve();
    }
  },
  nextSessionName: (prefix: string) => prefix,
}));

vi.mock("../terminal/transport.ts", () => ({
  TauriTransport: class {
    attach() {
      return Promise.resolve();
    }
    stdin() {
      return Promise.resolve();
    }
    resize() {
      return Promise.resolve();
    }
    selectPane() {
      return Promise.resolve();
    }
    zoomToggle() {
      return Promise.resolve();
    }
    splitPane() {
      return Promise.resolve();
    }
    closePane() {
      return Promise.resolve();
    }
    detach() {
      return Promise.resolve();
    }
  },
  routeMessage: () => {},
}));

/** Render the app and click the "work" row, leaving DOM focus on that
 * button exactly as a real click does. Returns the row button. */
async function openSession() {
  const view = render(<App />);
  const row = await view.findByRole("button", { name: /work/ });
  focusSpy.mockClear();
  await act(async () => {
    fireEvent.click(row);
  });
  return { view, row };
}

beforeEach(() => {
  focusSpy.mockClear();
});

// Without this every render accumulates in the same document and the row
// query matches several buttons — which fails as "multiple elements",
// masking whatever the test was actually asserting.
afterEach(() => cleanup());

describe("keyboard focus on first mount", () => {
  it("focuses a session opened from the URL, with no click anywhere", async () => {
    // The deep-link / restore path: ?session=work opens attached with the
    // user never having clicked, so nothing else can grant focus. This is
    // the case mount-time focus exists for, and it is the reason removing
    // that call must fail a test.
    const url = new URL(window.location.href);
    url.searchParams.set("session", "work");
    window.history.replaceState({}, "", url);
    try {
      focusSpy.mockClear();
      const view = render(<App />);
      await act(async () => {});
      expect(focusSpy).toHaveBeenCalled();
      expect(view.container.querySelector("textarea")).toBe(document.activeElement);
    } finally {
      url.searchParams.delete("session");
      window.history.replaceState({}, "", url);
    }
  });
});

describe("keyboard focus after a sidebar click", () => {
  it("moves focus off the sidebar button and into the terminal", async () => {
    const { row } = await openSession();
    // The precise failure being pinned: the click leaves focus on the
    // button, so keystrokes go to the button and the session sees nothing.
    expect(document.activeElement).not.toBe(row);
    expect(document.activeElement?.tagName).toBe("TEXTAREA");
  });

  it("refocuses when the ALREADY-active session is clicked again", async () => {
    const { row } = await openSession();
    // Simulate focus wandering (tab, a stray click on chrome).
    (row as HTMLElement).focus();
    expect(document.activeElement).toBe(row);

    focusSpy.mockClear();
    await act(async () => {
      fireEvent.click(row);
    });
    // No remount happens here — same session name — so mount-time focus
    // cannot save this case. Only the explicit refocus path can.
    expect(focusSpy).toHaveBeenCalled();
    expect(document.activeElement?.tagName).toBe("TEXTAREA");
  });

  it("takes focus back when the window regains it", async () => {
    const { row } = await openSession();
    (row as HTMLElement).focus();
    focusSpy.mockClear();
    await act(async () => {
      window.dispatchEvent(new Event("focus"));
    });
    expect(focusSpy).toHaveBeenCalled();
  });

  it("a click anywhere in the terminal region focuses it", async () => {
    const { view, row } = await openSession();
    (row as HTMLElement).focus();
    focusSpy.mockClear();
    // The letter-box margin belongs to the host div, not to xterm's own
    // element, so xterm would not self-focus from this click. The host is
    // the wrapper's first child (the overlay/toolbar are its siblings);
    // it is absolutely inset 0, so in a real window every click in the
    // region physically lands on it — fireEvent must target it directly
    // because DOM events bubble up, not down.
    const host = view.container.querySelector("main > div > div") as HTMLElement;
    expect(host).toBeTruthy();
    await act(async () => {
      fireEvent.click(host);
    });
    expect(focusSpy).toHaveBeenCalled();
  });
});
