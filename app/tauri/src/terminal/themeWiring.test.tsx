// @vitest-environment jsdom
//
// The constructor-and-subscription wiring TerminalView owns: xterm gets
// a resolved-hex theme at mount, and a theme change re-derives it. Pure
// mapping lives in xtermTheme.test.ts; what this pins is that the
// component actually passes it and actually re-applies it — the two
// lines a refactor silently drops.

import { afterEach, describe, expect, it, vi } from "vitest";
import { act, cleanup, render } from "@testing-library/react";
import TerminalView from "./Terminal";
import { applyTheme, DEFAULT_DARK } from "../theme";

interface CapturedTerm {
  ctorOpts: Record<string, unknown>;
  options: Record<string, unknown>;
}
let lastTerm: CapturedTerm | null = null;

vi.mock("@xterm/xterm", () => {
  class FakeTerm {
    element: HTMLElement | null = null;
    cols = 80;
    rows = 24;
    ctorOpts: Record<string, unknown>;
    options: Record<string, unknown> = {};
    constructor(opts: Record<string, unknown>) {
      this.ctorOpts = opts;
      lastTerm = this as unknown as CapturedTerm;
    }
    open(host: HTMLElement) {
      const el = document.createElement("div");
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
    attachCustomKeyEventHandler() {}
    onScroll() {
      return { dispose() {} };
    }
    onWriteParsed() {
      return { dispose() {} };
    }
  }
  return { Terminal: FakeTerm };
});
vi.mock("@xterm/xterm/css/xterm.css", () => ({}));

function transport() {
  return {
    attach: () => Promise.resolve(),
    stdin: () => Promise.resolve(),
    resize: () => Promise.resolve(),
    selectPane: () => Promise.resolve(),
    scrollbackReq: () => Promise.resolve(),
    zoomToggle: () => Promise.resolve(),
    splitPane: () => Promise.resolve(),
    closePane: () => Promise.resolve(),
    detach: () => Promise.resolve(),
  };
}

afterEach(() => {
  cleanup();
  lastTerm = null;
});

describe("terminal theme wiring", () => {
  it("constructs xterm with the resolved token theme", async () => {
    render(<TerminalView transport={transport() as never} session="s" onClosed={() => {}} />);
    await act(async () => {});
    const themeOpt = lastTerm!.ctorOpts.theme as Record<string, string>;
    // jsdom resolves no CSS, so the fallback tokens are what arrive —
    // and they must be concrete hex, never a var() string the canvas
    // renderer cannot evaluate.
    expect(themeOpt.background).toBe(DEFAULT_DARK.bgMain);
    expect(themeOpt.foreground).toBe(DEFAULT_DARK.text);
    expect(themeOpt.background.startsWith("#")).toBe(true);
  });

  it("re-derives the theme when the app theme changes, and stops after unmount", async () => {
    const view = render(
      <TerminalView transport={transport() as never} session="s" onClosed={() => {}} />,
    );
    await act(async () => {});
    const term = lastTerm!;
    expect(term.options.theme).toBeUndefined();
    await act(async () => applyTheme("dark"));
    expect((term.options.theme as Record<string, string>).background).toBe(DEFAULT_DARK.bgMain);
    // After unmount the subscription must be gone: poison the slot and
    // fire again — a live listener would overwrite it.
    term.options.theme = "poisoned";
    view.unmount();
    await act(async () => applyTheme("dark"));
    expect(term.options.theme).toBe("poisoned");
  });
});
