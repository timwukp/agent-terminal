// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from "vitest";
import { applyTheme, DEFAULT_DARK, onThemeChange, resolveTokens, theme } from "./theme";

afterEach(() => {
  delete document.documentElement.dataset.theme;
});

describe("theme accessor", () => {
  it("inline-style values are var() references, not raw hex", () => {
    // Raw hex here would freeze the theme at import time; var() is what
    // makes a future theme switch repaint inline styles for free.
    expect(theme.bg).toBe("var(--bg)");
    expect(theme.dangerStrong).toBe("var(--danger-strong)");
  });
});

describe("resolveTokens", () => {
  it("falls back per-token to DEFAULT_DARK when properties resolve empty", () => {
    // jsdom loads no stylesheet — exactly the "CSS missing" failure this
    // fallback exists for. Without it xterm gets background:"" (black).
    expect(resolveTokens()).toEqual(DEFAULT_DARK);
  });
});

describe("applyTheme / onThemeChange", () => {
  it("sets the data-theme attribute CSS blocks key off", () => {
    applyTheme("dark");
    expect(document.documentElement.dataset.theme).toBe("dark");
  });

  it("notifies subscribers, and a disposed subscriber never fires again", () => {
    const cb = vi.fn();
    const dispose = onThemeChange(cb);
    applyTheme("dark");
    expect(cb).toHaveBeenCalledTimes(1);
    dispose();
    applyTheme("dark");
    expect(cb).toHaveBeenCalledTimes(1);
  });
});
