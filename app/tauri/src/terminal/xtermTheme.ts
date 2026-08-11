// Derive xterm's ITheme from the design tokens, so the terminal cells
// and the chrome around them come from one palette — before this the
// constructor passed no theme and xterm rendered its default #000
// inside the #1e2228 letterbox: a visible seam exactly where the
// letterbox meets the cells.
//
// Deliberately NOT set: the 16 ANSI colors. Session content owns its
// SGR palette; theming it would repaint every CLI tool's output to
// match our chrome, which is the terminal equivalent of colorizing
// someone else's photographs.

import type { ITheme } from "@xterm/xterm";
import type { Tokens } from "../theme";

/** 35% alpha, appended to a #RRGGBB accent (xterm accepts #RRGGBBAA).
 * Solid-accent selection would hide the glyphs it selects. */
export const SELECTION_ALPHA = "59";

export function xtermTheme(t: Tokens): ITheme {
  return {
    background: t.bgMain,
    foreground: t.text,
    cursor: t.text,
    // The cell the block cursor sits on inverts to stay readable.
    cursorAccent: t.bgMain,
    selectionBackground: t.accent + SELECTION_ALPHA,
  };
}
