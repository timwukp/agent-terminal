// Typed access to the design tokens in theme.css (direction: Kiro
// Crew's dashboard — dark chrome, hairline borders, one restrained
// accent, all as CSS custom properties on [data-theme]).
//
// Two views of the same tokens, and the split is the point:
//  - `theme.*` values are `var(--…)` strings for inline styles/CSS —
//    they re-resolve on theme change for free.
//  - `resolveTokens()` returns concrete hex for consumers that cannot
//    evaluate var() — xterm's canvas renderer is the one that matters
//    (see terminal/xtermTheme.ts).
//
// The chart accent doubles as the single sparkline series color; it
// passes the dataviz palette validator on both this dark surface and
// light (lightness band, chroma, ≥3:1 contrast).

export interface Tokens {
  bg: string;
  bgMain: string;
  surface: string;
  border: string;
  text: string;
  textMuted: string;
  accent: string;
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
  text: "--text",
  textMuted: "--text-muted",
  accent: "--accent",
  good: "--good",
  danger: "--danger",
  dangerStrong: "--danger-strong",
};

/** The dark theme's raw values, duplicated from theme.css on purpose:
 * this is the per-token fallback when a custom property resolves empty
 * (stylesheet not loaded — jsdom in tests, or a broken asset build).
 * A terminal with a wrong-but-visible background beats an invisible
 * one. */
export const DEFAULT_DARK: Tokens = {
  bg: "#14171c",
  bgMain: "#1e2228",
  surface: "#1f242c",
  border: "#2d333d",
  text: "#dfe4ea",
  textMuted: "#8b949e",
  accent: "#2b6cb0",
  good: "#3fb950",
  danger: "#e5534b",
  dangerStrong: "#7f1d1d",
};

function varRefs(): Tokens {
  const out = {} as Record<keyof Tokens, string>;
  for (const k of Object.keys(CSS_VARS) as (keyof Tokens)[]) {
    out[k] = `var(${CSS_VARS[k]})`;
  }
  return out as Tokens;
}

/** Inline-style view: every value is a `var(--…)` reference. */
export const theme: Readonly<Tokens> = varRefs();

/** Concrete-hex view for non-CSS consumers (xterm). Reads the computed
 * custom properties off the root element; any token that resolves empty
 * falls back to DEFAULT_DARK for that token alone. */
export function resolveTokens(): Tokens {
  const style = getComputedStyle(document.documentElement);
  const out = {} as Record<keyof Tokens, string>;
  for (const k of Object.keys(CSS_VARS) as (keyof Tokens)[]) {
    const v = style.getPropertyValue(CSS_VARS[k]).trim();
    out[k] = v !== "" ? v : DEFAULT_DARK[k];
  }
  return out as Tokens;
}

export type ThemeName = "dark";

const THEME_EVENT = "themechange";

/** Select a theme: flips [data-theme] (CSS re-resolves every var()
 * instantly) and notifies non-CSS consumers to re-derive from
 * resolveTokens(). Called once at startup (main.tsx) and by any future
 * theme switcher. */
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
