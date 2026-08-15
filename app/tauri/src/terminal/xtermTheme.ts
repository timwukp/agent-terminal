// Derive xterm's ITheme from the design tokens, so the terminal cells
// and the chrome around them come from one palette — before this the
// constructor passed no theme and xterm rendered its default #000
// inside the #1e2228 letterbox: a visible seam exactly where the
// letterbox meets the cells.

import type { ITheme } from "@xterm/xterm";
import type { ThemeName, Tokens } from "../theme";

/** 35% alpha, appended to a #RRGGBB accent (xterm accepts #RRGGBBAA).
 * Solid-accent selection would hide the glyphs it selects. */
export const SELECTION_ALPHA = "59";

/** The 16 ANSI slots, re-picked for a white background.
 *
 * Dark mode deliberately sets NONE of these: session content owns its
 * SGR palette, xterm's built-in one already assumes a dark surface, and
 * repainting it to match our chrome is the terminal equivalent of
 * colorizing someone else's photographs.
 *
 * Light mode gets no such free pass, because xterm's defaults are not
 * neutral — they are dark-surface values. Measured against #ffffff:
 * `white` (#d3d7cf) is 1.46:1 and `brightWhite` (#eeeeec) is 1.16:1, i.e.
 * invisible; `brightCyan` (#34e2e2) is 1.60:1. On a light theme those are
 * not aesthetic complaints, they are output the user cannot read. So
 * light ships a palette, and every step below measures ≥4.5:1 on
 * #ffffff (asserted in xtermTheme.test.ts).
 *
 * The white pair inverts on purpose: with light ink on a dark surface
 * "brighter" means lighter, and on a light surface it means MORE INK. A
 * tool printing bright-white for emphasis still gets emphasis; one
 * printing it to mean the color white does not, and there is no palette
 * where both readings survive. */
export const ANSI_LIGHT: Readonly<Record<string, string>> = {
  black: "#1f2328",
  red: "#cf222e",
  green: "#1a7f37",
  yellow: "#7d4e00",
  blue: "#0969da",
  magenta: "#8250df",
  cyan: "#1b7c83",
  white: "#6e7781",
  brightBlack: "#57606a",
  brightRed: "#a40e26",
  brightGreen: "#116329",
  brightYellow: "#633c01",
  brightBlue: "#0550ae",
  brightMagenta: "#6639ba",
  brightCyan: "#0e6b70",
  brightWhite: "#010409",
};

/** `name` is required rather than read off the document: this stays a
 * pure function of its inputs, which is also what lets the test assert
 * both palettes without a DOM. */
export function xtermTheme(t: Tokens, name: ThemeName): ITheme {
  return {
    background: t.bgMain,
    foreground: t.text,
    cursor: t.text,
    // The cell the block cursor sits on inverts to stay readable.
    cursorAccent: t.bgMain,
    selectionBackground: t.accent + SELECTION_ALPHA,
    ...(name === "light" ? ANSI_LIGHT : {}),
  };
}
