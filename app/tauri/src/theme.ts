// Typed access to the design tokens in theme.css (direction: Kiro
// Crew's dashboard — hairline borders, one restrained accent, all as CSS
// custom properties on [data-theme]).
//
// Two views of the same tokens, and the split is the point:
//  - `theme.*` values are `var(--…)` strings for inline styles/CSS —
//    they re-resolve on theme change for free.
//  - `resolveTokens()` returns concrete hex for consumers that cannot
//    evaluate var() — xterm's canvas renderer is the one that matters
//    (see terminal/xtermTheme.ts).
//
// Two themes ship: dark (the default) and light. `accent` and `onAccent`
// are the only values they share, because `accent` carries white ink at
// 5.42:1 on both — that is what makes one blue usable as a fill in
// either. Every other ink role had to be re-picked: dark's `good`
// #3fb950 is 2.24:1 on light `bg`, and dark's own `danger` was 4.21:1 on
// `surface` (under AA) until this pass moved it to #ec6a62 at 5.05:1.
//
// `accent` is a FILL and a chart series, never text: as a series on `bg`
// it is 3.31:1 dark / 4.78:1 light, and its one fill-on-`bgMain` use
// (the jump-to-bottom pill) measures 2.95:1 — that pill is identified by
// its 5.42:1 white label, not by its edge, so the 3:1 non-text floor
// applies to the label's box rather than to the fill. Fills that DO need
// their own edge use `focusRing` (5.80:1 dark / 5.19:1 light on
// `bgMain`).
//
// Every ratio above is computed, not eyeballed: theme.test.ts runs the
// pairs through contrast.ts, so a token edit that breaks one fails there.

export interface Tokens {
  bg: string;
  bgMain: string;
  surface: string;
  border: string;
  /** Fill + border of a control that sits ON the terminal surface (the
   * pane toolbar). One step away from `surface`/`border`, because a
   * button that borrowed those was invisible against `bgMain`. */
  raised: string;
  raisedBorder: string;
  text: string;
  textMuted: string;
  accent: string;
  /** Ink on an `accent`/`danger`/`dangerStrong` fill. White in both
   * themes: the fills are mid-to-dark in both, so this one does not
   * flip. */
  onAccent: string;
  /** The active pane's ring. Brighter than `accent` on purpose — it has
   * to read against the terminal background, not against chrome. */
  focusRing: string;
  good: string;
  danger: string;
  dangerStrong: string;
}

/** CSS property name per token — single source for both views. */
const CSS_VARS: Record<keyof Tokens, string> = {
  bg: "--bg",
  bgMain: "--bg-main",
  surface: "--surface",
  border: "--border",
  raised: "--raised",
  raisedBorder: "--raised-border",
  text: "--text",
  textMuted: "--text-muted",
  accent: "--accent",
  onAccent: "--on-accent",
  focusRing: "--focus-ring",
  good: "--good",
  danger: "--danger",
  dangerStrong: "--danger-strong",
};

/** Each theme's raw values, duplicated from theme.css on purpose: this is
 * the per-token fallback when a custom property resolves empty
 * (stylesheet not loaded — jsdom in tests, or a broken asset build). A
 * terminal with a wrong-but-visible background beats an invisible one.
 * theme.test.ts reads theme.css and asserts these agree with it, so the
 * duplication cannot drift silently. */
export const DEFAULT_DARK: Tokens = {
  bg: "#14171c",
  bgMain: "#1e2228",
  surface: "#1f242c",
  border: "#2d333d",
  raised: "#2d3239",
  raisedBorder: "#3f464f",
  text: "#dfe4ea",
  textMuted: "#8b949e",
  accent: "#2b6cb0",
  onAccent: "#ffffff",
  focusRing: "#4a9eff",
  good: "#3fb950",
  danger: "#ec6a62",
  dangerStrong: "#7f1d1d",
};

export const DEFAULT_LIGHT: Tokens = {
  bg: "#eef1f5",
  bgMain: "#ffffff",
  surface: "#f6f8fa",
  border: "#d0d7de",
  raised: "#f0f3f6",
  raisedBorder: "#afb8c1",
  text: "#1f2328",
  textMuted: "#59636e",
  accent: "#2b6cb0",
  onAccent: "#ffffff",
  focusRing: "#0969da",
  good: "#0f7a2f",
  danger: "#cf222e",
  dangerStrong: "#a40e26",
};

export type ThemeName = "dark" | "light";

/** What the user picked. "system" is a third state, not a synonym for
 * whichever theme the OS is on right now: it keeps following the OS
 * afterwards (ux-spec.md's "following the OS"). */
export type ThemePref = ThemeName | "system";

export const DEFAULTS: Readonly<Record<ThemeName, Tokens>> = {
  dark: DEFAULT_DARK,
  light: DEFAULT_LIGHT,
};

const PREF_KEY = "agent-terminal.theme";
const LIGHT_QUERY = "(prefers-color-scheme: light)";

function varRefs(): Tokens {
  const out = {} as Record<keyof Tokens, string>;
  for (const k of Object.keys(CSS_VARS) as (keyof Tokens)[]) {
    out[k] = `var(${CSS_VARS[k]})`;
  }
  return out as Tokens;
}

/** Inline-style view: every value is a `var(--…)` reference. */
export const theme: Readonly<Tokens> = varRefs();

/** Which theme is on the document right now. Anything unrecognized reads
 * as "dark" — the same answer as no attribute at all, because both mean
 * "no theme block matched" and dark is what ships by default. */
export function currentTheme(): ThemeName {
  const v = document.documentElement.dataset.theme;
  return v === "light" || v === "dark" ? v : "dark";
}

/** Concrete-hex view for non-CSS consumers (xterm). Reads the computed
 * custom properties off the root element; any token that resolves empty
 * falls back to the CURRENT theme's defaults for that token alone —
 * falling back to dark under a light document would put dark ink on a
 * light surface, one token at a time. */
export function resolveTokens(): Tokens {
  const fallback = DEFAULTS[currentTheme()];
  const style = getComputedStyle(document.documentElement);
  const out = {} as Record<keyof Tokens, string>;
  for (const k of Object.keys(CSS_VARS) as (keyof Tokens)[]) {
    const v = style.getPropertyValue(CSS_VARS[k]).trim();
    out[k] = v !== "" ? v : fallback[k];
  }
  return out as Tokens;
}

const THEME_EVENT = "themechange";

/** Select a theme: flips [data-theme] (CSS re-resolves every var()
 * instantly) and notifies non-CSS consumers to re-derive from
 * resolveTokens(). Prefer applyPref()/initTheme() — this is the layer
 * under them, and it deliberately does not persist anything. */
export function applyTheme(name: ThemeName): void {
  document.documentElement.dataset.theme = name;
  document.dispatchEvent(new CustomEvent(THEME_EVENT, { detail: name }));
}

/** Subscribe to theme changes. Returns a disposer — a listener from an
 * unmounted terminal writing to a disposed xterm is the bug this
 * signature exists to prevent. */
export function onThemeChange(cb: () => void): () => void {
  document.addEventListener(THEME_EVENT, cb);
  return () => document.removeEventListener(THEME_EVENT, cb);
}

function mediaQuery(): MediaQueryList | null {
  // Hosts without matchMedia exist (jsdom variants, older webviews).
  // "Cannot ask the OS" is not an error worth failing startup over; it
  // resolves to the shipped default.
  if (typeof window === "undefined" || typeof window.matchMedia !== "function") return null;
  return window.matchMedia(LIGHT_QUERY);
}

/** What the OS is asking for, or "dark" when it cannot be asked. */
export function systemTheme(): ThemeName {
  return mediaQuery()?.matches === true ? "light" : "dark";
}

/** Web Storage through `window`, never the bare global: Node 22+ defines a
 * `globalThis.localStorage` of its own, and reaching it instead of the
 * document's made every preference test fail on a method that isn't there. */
function storage(): Storage | null {
  if (typeof window === "undefined") return null;
  return window.localStorage ?? null;
}

/** The stored preference. Unrecognized or unreadable storage reads as
 * "system": a corrupt value must not pin the app to a theme the user
 * cannot see the reason for. */
export function loadPref(): ThemePref {
  try {
    const v = storage()?.getItem(PREF_KEY);
    if (v === "dark" || v === "light" || v === "system") return v;
  } catch {
    // Storage can throw outright (disabled, or over quota).
  }
  return "system";
}

export function savePref(pref: ThemePref): void {
  try {
    storage()?.setItem(PREF_KEY, pref);
  } catch {
    // Losing the preference across restarts beats not switching at all.
  }
}

export function resolvePref(pref: ThemePref): ThemeName {
  return pref === "system" ? systemTheme() : pref;
}

/** Persist a preference and apply it. Returns the theme it resolved to. */
export function applyPref(pref: ThemePref): ThemeName {
  savePref(pref);
  const name = resolvePref(pref);
  applyTheme(name);
  return name;
}

/** Startup: apply the stored preference, then keep following the OS for
 * as long as the preference IS "system" — the listener re-reads it on
 * every change rather than capturing it, so a later switch to an explicit
 * theme stops the following without needing to unsubscribe. Returns a
 * disposer. */
export function initTheme(): () => void {
  applyTheme(resolvePref(loadPref()));
  const mq = mediaQuery();
  if (mq === null) return () => {};
  const onChange = () => {
    if (loadPref() === "system") applyTheme(systemTheme());
  };
  if (typeof mq.addEventListener === "function") {
    mq.addEventListener("change", onChange);
    return () => mq.removeEventListener("change", onChange);
  }
  // Safari < 14 / old WKWebView: MediaQueryList predates EventTarget.
  mq.addListener(onChange);
  return () => mq.removeListener(onChange);
}
