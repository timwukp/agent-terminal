// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
// `?raw` rather than node:fs: the app ships no @types/node (see
// capabilities.test.ts), and the bundler that builds the app resolves this.
import themeCss from "./theme.css?raw";
import { contrastRatio } from "./contrast";
import {
  applyPref,
  applyTheme,
  currentTheme,
  DEFAULT_DARK,
  DEFAULT_LIGHT,
  DEFAULTS,
  initTheme,
  loadPref,
  onThemeChange,
  resolvePref,
  resolveTokens,
  savePref,
  systemTheme,
  theme,
  type ThemeName,
  type Tokens,
} from "./theme";

const NAMES: readonly ThemeName[] = ["dark", "light"];

afterEach(() => {
  delete document.documentElement.dataset.theme;
  // Unstub BEFORE clearing: one test replaces storage with one that throws,
  // and clearing that one would fail the teardown of every later test.
  vi.unstubAllGlobals();
  window.localStorage.clear();
});

describe("theme accessor", () => {
  it("inline-style values are var() references, not raw hex", () => {
    // Raw hex here would freeze the theme at import time; var() is what
    // makes a theme switch repaint inline styles for free.
    expect(theme.bg).toBe("var(--bg)");
    expect(theme.dangerStrong).toBe("var(--danger-strong)");
  });
});

describe("theme.css and the TS defaults", () => {
  // The fallback tables in theme.ts duplicate the stylesheet on purpose.
  // Duplication is only safe while something checks it, and "something"
  // cannot be a human reading two files.
  const css = themeCss;

  it("the stylesheet under test was actually loaded", () => {
    // Not ceremony: vitest stubs CSS imports to "" unless `css: true` is
    // set (vite.config.ts), and an empty stylesheet parses to zero tokens
    // — every check below would then pass by finding nothing to compare.
    expect(css.length).toBeGreaterThan(500);
    expect(css).toContain(':root[data-theme="dark"]');
  });

  /** The `--x: #hex;` pairs inside one :root[data-theme="…"] block. */
  const block = (name: ThemeName): Map<string, string> => {
    const start = css.indexOf(`:root[data-theme="${name}"] {`);
    expect(start, `no CSS block for the "${name}" theme`).toBeGreaterThanOrEqual(0);
    const body = css.slice(start, css.indexOf("}", start));
    const out = new Map<string, string>();
    for (const m of body.matchAll(/(--[a-z-]+):\s*([^;]+);/g)) out.set(m[1], m[2].trim());
    return out;
  };

  it.each(NAMES)("%s defines every token, and nothing extra", (name) => {
    const declared = block(name);
    const wanted = new Set(
      (Object.keys(DEFAULTS[name]) as (keyof Tokens)[]).map((k) =>
        // `theme` is the var() view, so it is the mapping itself.
        theme[k].replace(/^var\(|\)$/g, ""),
      ),
    );
    expect(new Set(declared.keys())).toEqual(wanted);
  });

  it.each(NAMES)("%s's CSS values are exactly the TS fallbacks", (name) => {
    const declared = block(name);
    for (const k of Object.keys(DEFAULTS[name]) as (keyof Tokens)[]) {
      const cssVar = theme[k].replace(/^var\(|\)$/g, "");
      expect(declared.get(cssVar), `${name} ${k}`).toBe(DEFAULTS[name][k]);
    }
  });

  it("keeps a pre-paint background for both OS appearances", () => {
    // [data-theme] cannot be set before the first paint (module code), and
    // no inline script is allowed to fix that (CSP). Without these two
    // rules the window opens on the UA's white.
    expect(css).toMatch(/html\s*{\s*background:\s*#14171c;/);
    expect(css).toMatch(/prefers-color-scheme:\s*light\)\s*{\s*html\s*{\s*background:\s*#eef1f5;/);
  });
});

describe("token contrast (WCAG 2.1, computed not eyeballed)", () => {
  const AA_TEXT = 4.5;
  const NON_TEXT = 3;

  /** Every pairing that actually occurs in the UI, with the floor that
   * applies to it. Deliberately NOT exhaustive over the cross product: a
   * pair no component renders would be an invented requirement, and the
   * first thing a maintainer would do about a failing invented pair is
   * weaken the floor for the real ones too. */
  const pairs = (t: Tokens): [string, string, string, number][] => [
    ["text on the terminal", t.text, t.bgMain, AA_TEXT],
    ["text on a panel row", t.text, t.surface, AA_TEXT],
    ["text on the chrome", t.text, t.bg, AA_TEXT],
    ["text on a pane-toolbar button", t.text, t.raised, AA_TEXT],
    ["muted text on a panel row", t.textMuted, t.surface, AA_TEXT],
    ["muted text on the chrome", t.textMuted, t.bg, AA_TEXT],
    ["good on a panel row", t.good, t.surface, AA_TEXT],
    ["good on the chrome", t.good, t.bg, AA_TEXT],
    ["danger text on a panel row", t.danger, t.surface, AA_TEXT],
    ["danger text on the chrome", t.danger, t.bg, AA_TEXT],
    ["onAccent ink on an accent fill", t.onAccent, t.accent, AA_TEXT],
    ["onAccent ink on a destructive fill", t.onAccent, t.dangerStrong, AA_TEXT],
    ["sparkline series on the chrome", t.accent, t.bg, NON_TEXT],
    ["active-pane ring on the terminal", t.focusRing, t.bgMain, NON_TEXT],
  ];

  it.each(NAMES)("%s clears every floor", (name) => {
    const failures = pairs(DEFAULTS[name])
      .map(([label, ink, surface, floor]) => ({
        label,
        floor,
        ratio: Number(contrastRatio(ink, surface).toFixed(2)),
      }))
      .filter((r) => r.ratio < r.floor);
    expect(failures).toEqual([]);
  });

  it("keeps the refuted values pinned, so a revert would not read as fine", () => {
    // Both were candidates this pass rejected on measurement, and both are
    // quoted in theme.ts. Without them here, restoring either would leave
    // the comment describing a ratio nothing computes.
    const at = (ink: string, surface: string) => Number(contrastRatio(ink, surface).toFixed(2));
    expect(at("#e5534b", DEFAULT_DARK.surface)).toBe(4.21); // dark danger, before
    expect(at(DEFAULT_DARK.danger, DEFAULT_DARK.surface)).toBe(5.05); // and after
    expect(at("#1a7f37", DEFAULT_LIGHT.bg)).toBe(4.48); // light good, rejected
    expect(at(DEFAULT_LIGHT.good, DEFAULT_LIGHT.bg)).toBe(4.82); // and chosen
    expect(at(DEFAULT_DARK.good, DEFAULT_LIGHT.bg)).toBe(2.24); // why light re-picks
    expect(at(DEFAULT_DARK.onAccent, DEFAULT_DARK.accent)).toBe(5.42); // shared accent
    // The accent-is-never-text policy, in numbers: as a series it clears the
    // 3:1 non-text floor on `bg` in both themes, and its one fill use on
    // `bgMain` does NOT — which is why that pill is identified by its label.
    expect(at(DEFAULT_DARK.accent, DEFAULT_DARK.bg)).toBe(3.31);
    expect(at(DEFAULT_LIGHT.accent, DEFAULT_LIGHT.bg)).toBe(4.78);
    expect(at(DEFAULT_DARK.accent, DEFAULT_DARK.bgMain)).toBe(2.95);
    expect(at(DEFAULT_DARK.focusRing, DEFAULT_DARK.bgMain)).toBe(5.8);
    expect(at(DEFAULT_LIGHT.focusRing, DEFAULT_LIGHT.bgMain)).toBe(5.19);
    // Why the kill button uses the strong step (Sidebar.tsx): `danger` is
    // an ink, and white on it fails in dark.
    expect(at(DEFAULT_DARK.onAccent, DEFAULT_DARK.danger)).toBe(3.09);
    expect(at(DEFAULT_DARK.onAccent, DEFAULT_DARK.dangerStrong)).toBe(10.02);
  });

  it("the floors are reachable — a deliberately bad pair fails", () => {
    // Reverse control: without this, a bug that made contrastRatio()
    // return 21 for everything would leave the suite green.
    expect(contrastRatio(DEFAULT_DARK.text, DEFAULT_LIGHT.bgMain)).toBeLessThan(AA_TEXT);
    expect(contrastRatio(DEFAULT_LIGHT.good, DEFAULT_DARK.bg)).toBeLessThan(AA_TEXT);
  });
});

describe("currentTheme", () => {
  it("reads the attribute, and treats anything unrecognized as dark", () => {
    expect(currentTheme()).toBe("dark"); // no attribute at all
    applyTheme("light");
    expect(currentTheme()).toBe("light");
    document.documentElement.dataset.theme = "solarized";
    expect(currentTheme()).toBe("dark");
  });
});

describe("resolveTokens", () => {
  it("falls back per-token to the CURRENT theme, not always to dark", () => {
    // jsdom loads no stylesheet — exactly the "CSS missing" failure this
    // fallback exists for. Falling back to dark under a light document
    // would put #dfe4ea text on a white surface, one token at a time.
    expect(resolveTokens()).toEqual(DEFAULT_DARK);
    applyTheme("light");
    expect(resolveTokens()).toEqual(DEFAULT_LIGHT);
  });
});

describe("applyTheme / onThemeChange", () => {
  it("sets the data-theme attribute CSS blocks key off", () => {
    applyTheme("light");
    expect(document.documentElement.dataset.theme).toBe("light");
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

/** A controllable `(prefers-color-scheme: light)`. */
function stubMatchMedia(matches: boolean) {
  const listeners = new Set<() => void>();
  const mq = {
    matches,
    media: "(prefers-color-scheme: light)",
    addEventListener: (_: string, cb: () => void) => void listeners.add(cb),
    removeEventListener: (_: string, cb: () => void) => void listeners.delete(cb),
  };
  vi.stubGlobal(
    "matchMedia",
    vi.fn(() => mq),
  );
  return {
    /** What the OS now says, plus the change event it would fire. */
    set(next: boolean) {
      mq.matches = next;
      for (const cb of listeners) cb();
    },
    listenerCount: () => listeners.size,
  };
}

describe("the stored preference", () => {
  beforeEach(() => stubMatchMedia(false));

  it("defaults to system, and a corrupt value reads as system too", () => {
    expect(loadPref()).toBe("system");
    window.localStorage.setItem("agent-terminal.theme", "sepia");
    expect(loadPref()).toBe("system");
  });

  it("round-trips an explicit choice", () => {
    savePref("light");
    expect(loadPref()).toBe("light");
  });

  it("survives storage that throws outright", () => {
    vi.stubGlobal("localStorage", {
      getItem: () => {
        throw new Error("denied");
      },
      setItem: () => {
        throw new Error("denied");
      },
    });
    expect(loadPref()).toBe("system");
    expect(() => savePref("dark")).not.toThrow();
  });

  it("applyPref persists and applies in one step", () => {
    expect(applyPref("light")).toBe("light");
    expect(document.documentElement.dataset.theme).toBe("light");
    expect(loadPref()).toBe("light");
  });
});

describe("system following", () => {
  it("systemTheme reports what the OS asks for", () => {
    const os = stubMatchMedia(true);
    expect(systemTheme()).toBe("light");
    os.set(false);
    expect(systemTheme()).toBe("dark");
  });

  it("resolves to dark when the OS cannot be asked at all", () => {
    // No matchMedia: some webviews and jsdom builds. "Cannot ask" must
    // not throw during startup.
    vi.stubGlobal("matchMedia", undefined);
    expect(systemTheme()).toBe("dark");
    expect(resolvePref("system")).toBe("dark");
    expect(resolvePref("light")).toBe("light");
  });

  it("initTheme applies the stored preference", () => {
    stubMatchMedia(false);
    savePref("light");
    const dispose = initTheme();
    expect(document.documentElement.dataset.theme).toBe("light");
    dispose();
  });

  it("keeps following the OS while the preference is system", () => {
    const os = stubMatchMedia(false);
    const dispose = initTheme();
    expect(currentTheme()).toBe("dark");
    os.set(true);
    expect(currentTheme()).toBe("light");
    dispose();
  });

  it("stops following once an explicit theme is chosen — without unsubscribing", () => {
    // The listener re-reads the preference on every change rather than
    // capturing it. Mutation: capture it instead, and an OS flip after
    // applyPref("dark") repaints the app the user just pinned.
    const os = stubMatchMedia(false);
    const dispose = initTheme();
    applyPref("dark");
    os.set(true);
    expect(currentTheme()).toBe("dark");
    // ...and switching back to system resumes following immediately.
    applyPref("system");
    expect(currentTheme()).toBe("light");
    dispose();
  });

  it("the disposer removes the OS listener", () => {
    const os = stubMatchMedia(false);
    const dispose = initTheme();
    expect(os.listenerCount()).toBe(1);
    dispose();
    expect(os.listenerCount()).toBe(0);
    os.set(true);
    expect(currentTheme()).toBe("dark");
  });
});
