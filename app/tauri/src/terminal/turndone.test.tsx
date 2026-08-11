// @vitest-environment jsdom
//
// The turn-done path end to end through the components: a turn_done ctrl
// event (or a bell) in, an onTurnDone call out with the last non-empty
// screen line. The policy itself is enumerated in notify.test.ts; what
// this pins is the wiring TerminalView owns — including the latest-ref
// discipline that keeps the attach effect from re-attaching, which is
// exactly the kind of defect that passes every pure-function test.

import { describe, expect, it, vi, afterEach } from "vitest";
import { render, act, cleanup } from "@testing-library/react";
import TerminalView from "./Terminal";

type BellHandler = () => void;
let lastBell: BellHandler | null = null;

vi.mock("@xterm/xterm", () => {
  class FakeTerm {
    element: HTMLElement | null = null;
    cols = 80;
    rows = 24;
    _core = { _renderService: { dimensions: { css: { cell: { width: 8, height: 16 } } } } };
    // A tiny screen: two content rows, then blanks — lastNonEmptyLine
    // must skip the blanks and return "$ make test".
    buffer = {
      active: {
        length: 4,
        getLine: (y: number) => ({
          translateToString: () => ["build ok", "$ make test", "", ""][y] ?? "",
        }),
      },
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
    onBell(h: BellHandler) {
      lastBell = h;
      return { dispose() {} };
    }
  }
  return { Terminal: FakeTerm };
});
vi.mock("@xterm/xterm/css/xterm.css", () => ({}));

type Ctrl = (ev: unknown) => void;

function makeTransport() {
  let ctrl: Ctrl = () => {};
  const t = {
    attach: vi.fn((_s: string, _c: number, _r: number, ev: { onCtrl: Ctrl }) => {
      ctrl = ev.onCtrl;
      return Promise.resolve();
    }),
    stdin: () => Promise.resolve(),
    resize: () => Promise.resolve(),
    selectPane: () => Promise.resolve(),
    zoomToggle: () => Promise.resolve(),
    splitPane: () => Promise.resolve(),
    closePane: () => Promise.resolve(),
    detach: () => Promise.resolve(),
  };
  return { t, ctrl: (ev: unknown) => ctrl(ev) };
}

afterEach(() => {
  cleanup();
  lastBell = null;
});

describe("turn_done wiring", () => {
  it("a turn_done ctrl event reaches onTurnDone with the last non-empty line", async () => {
    const { t, ctrl } = makeTransport();
    const spy = vi.fn();
    render(
      <TerminalView transport={t as never} session="s" onClosed={() => {}} onTurnDone={spy} />,
    );
    await act(async () => {});
    await act(async () => ctrl({ kind: "turn_done" }));
    expect(spy).toHaveBeenCalledWith("idle", "$ make test");
  });

  it("a bell reaches onTurnDone the same way", async () => {
    const { t } = makeTransport();
    const spy = vi.fn();
    render(
      <TerminalView transport={t as never} session="s" onClosed={() => {}} onTurnDone={spy} />,
    );
    await act(async () => {});
    expect(lastBell).not.toBeNull();
    await act(async () => lastBell!());
    expect(spy).toHaveBeenCalledWith("bell", "$ make test");
  });

  it("a handler swap does NOT re-attach, and the NEW handler is the one called", async () => {
    // The latest-ref contract, both halves. Re-attaching per render drops
    // the connection (user-visible flicker + a daemon churn); calling the
    // old closure notifies from stale mute state.
    const { t, ctrl } = makeTransport();
    const first = vi.fn();
    const second = vi.fn();
    // onClosed must keep one identity across renders — it IS an attach
    // effect dep (like App's stable setClosed). An inline arrow here would
    // re-attach for a reason unrelated to what this test pins.
    const onClosed = () => {};
    const view = render(
      <TerminalView transport={t as never} session="s" onClosed={onClosed} onTurnDone={first} />,
    );
    await act(async () => {});
    view.rerender(
      <TerminalView transport={t as never} session="s" onClosed={onClosed} onTurnDone={second} />,
    );
    await act(async () => ctrl({ kind: "turn_done" }));
    expect(t.attach).toHaveBeenCalledTimes(1);
    expect(first).not.toHaveBeenCalled();
    expect(second).toHaveBeenCalledTimes(1);
  });
});
