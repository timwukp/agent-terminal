// @vitest-environment jsdom
//
// App's notification gate: turn_done in, badge + (maybe) OS notification
// out. Policy truth table lives in notify.test.ts and Terminal's event
// routing in turndone.test.tsx; this covers the seam App owns — reading
// focus at event time, the mute set, the ✓ badge, and clearing it when
// the session is selected (looking at a session serves its mark).

import { describe, expect, it, vi, afterEach, beforeEach } from "vitest";
import { render, fireEvent, act, cleanup } from "@testing-library/react";
import App from "./App";
import { deliverNotification } from "./notify";

vi.mock("./notify", async (importOriginal) => ({
  ...(await importOriginal<typeof import("./notify")>()),
  deliverNotification: vi.fn(() => Promise.resolve(true)),
}));

let ctrl: (ev: unknown) => void = () => {};

vi.mock("@xterm/xterm", () => {
  class FakeTerm {
    element: HTMLElement | null = null;
    cols = 80;
    rows = 24;
    buffer = {
      active: { length: 1, getLine: () => ({ translateToString: () => "done: 3 files" }) },
    };
    open(host: HTMLElement) {
      const el = document.createElement("div");
      el.appendChild(document.createElement("textarea"));
      host.appendChild(el);
      this.element = el;
    }
    focus() {}
    write() {}
    resize() {}
    scrollToBottom() {}
    dispose() {}
    onData() {
      return { dispose() {} };
    }
    onBell() {
      return { dispose() {} };
    }
  }
  return { Terminal: FakeTerm };
});
vi.mock("@xterm/xterm/css/xterm.css", () => ({}));

vi.mock("./sidebar/api.ts", () => ({
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

vi.mock("./terminal/transport.ts", () => ({
  TauriTransport: class {
    attach(_s: string, _c: number, _r: number, events: { onCtrl: (ev: unknown) => void }) {
      ctrl = events.onCtrl;
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

async function openWork() {
  const view = render(<App />);
  const row = await view.findByRole("button", { name: /^work/ });
  await act(async () => {
    fireEvent.click(row);
  });
  return view;
}

beforeEach(() => vi.mocked(deliverNotification).mockClear());
afterEach(() => cleanup());

describe("App notification gate", () => {
  it("unfocused turn_done: OS notification with the last line, and a ✓ on the row", async () => {
    const view = await openWork();
    vi.spyOn(document, "hasFocus").mockReturnValue(false);
    await act(async () => ctrl({ kind: "turn_done" }));
    expect(deliverNotification).toHaveBeenCalledWith("work", "done: 3 files");
    expect(view.getByTitle("finished while you were away")).toBeTruthy();
  });

  it("focused turn_done: nothing at all — the user is watching", async () => {
    const view = await openWork();
    vi.spyOn(document, "hasFocus").mockReturnValue(true);
    await act(async () => ctrl({ kind: "turn_done" }));
    expect(deliverNotification).not.toHaveBeenCalled();
    expect(view.queryByTitle("finished while you were away")).toBeNull();
  });

  it("muted: no notification, but the ✓ still records the finish", async () => {
    const view = await openWork();
    fireEvent.click(view.getByRole("button", { name: "mute notifications for work" }));
    vi.spyOn(document, "hasFocus").mockReturnValue(false);
    await act(async () => ctrl({ kind: "turn_done" }));
    expect(deliverNotification).not.toHaveBeenCalled();
    expect(view.getByTitle("finished while you were away")).toBeTruthy();
  });

  it("selecting the session clears its ✓", async () => {
    const view = await openWork();
    vi.spyOn(document, "hasFocus").mockReturnValue(false);
    await act(async () => ctrl({ kind: "turn_done" }));
    expect(view.getByTitle("finished while you were away")).toBeTruthy();
    await act(async () => {
      fireEvent.click(view.getByRole("button", { name: /^work/ }));
    });
    expect(view.queryByTitle("finished while you were away")).toBeNull();
  });
});
