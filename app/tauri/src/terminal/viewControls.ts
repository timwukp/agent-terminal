// View-only controls: font zoom and scroll position. Pure logic here,
// wiring in Terminal.tsx/App.tsx — these decisions are exactly the kind
// that rot into "works when I tried it" without table tests.

import { displayName } from "../displayName";

/** Font sizes a terminal stays readable at. The default matches the
 * Terminal.tsx constructor. */
export const FONT_MIN = 9;
export const FONT_DEFAULT = 13;
export const FONT_MAX = 24;

export type ZoomAction = "in" | "out" | "reset";

/** Map one keyboard event to a zoom action, or null to let xterm have
 * the key. Cmd on macOS, Ctrl elsewhere — both accepted rather than
 * platform-sniffed, since Ctrl+= is not a sequence a shell child ever
 * receives anyway (it has no single-byte encoding). The caller acts on
 * keydown only but must swallow every event type of a matched chord, so
 * this classifies the chord without looking at ev.type. */
export function zoomActionForKey(ev: {
  key: string;
  metaKey: boolean;
  ctrlKey: boolean;
}): ZoomAction | null {
  if (!ev.metaKey && !ev.ctrlKey) return null;
  if (ev.key === "+" || ev.key === "=") return "in";
  if (ev.key === "-") return "out";
  if (ev.key === "0") return "reset";
  return null;
}

/** One zoom step, clamped so the terminal can neither vanish nor become
 * three glyphs wide. */
export function nextFontSize(current: number, action: ZoomAction): number {
  if (action === "reset") return FONT_DEFAULT;
  const next = action === "in" ? current + 1 : current - 1;
  return Math.min(FONT_MAX, Math.max(FONT_MIN, next));
}

/** True when the viewport shows the live bottom of the buffer. viewportY
 * is the buffer line at the top of the viewport; baseY is that same line
 * when fully scrolled down — equal means nothing is hidden below. */
export function isAtBottom(viewportY: number, baseY: number): boolean {
  return viewportY >= baseY;
}

/** Lines of history that exist in the session but are NOT in this
 * terminal's buffer — everything older than the oldest line backfill
 * wrote.
 *
 * `sbLines` is the snapshot's total; `oldestLive` is
 * Backfill.oldestWritten() (null when no history page landed, in which
 * case the whole log is out of reach rather than none of it — the
 * direction a "did it write anything?" boolean gets backwards).
 *
 * This is the number the user is owed: their report was "I scrolled up and
 * my earlier conversation was not there", and the honest answer names how
 * many lines are beyond the buffer rather than implying the session was
 * restarted or cleared. Zero means the terminal really does hold all of
 * it, and the deep-history affordance must stay hidden — offering to open
 * a viewer onto nothing is its own kind of lie. */
export function unreachedHistory(sbLines: number, oldestLive: number | null): number {
  if (sbLines <= 0) return 0;
  return Math.max(0, Math.min(sbLines, oldestLive ?? sbLines));
}

/** Window title: the active session first (that is what ⌘-Tab and
 * screenshots need), the app name as suffix.
 *
 * The name is filtered for display. This composition puts our own app name
 * downstream of a daemon-supplied string, so a name carrying U+202E reverses
 * the suffix with it and the window in ⌘-Tab stops identifying the app —
 * same shape as the notification title (../displayName.ts). */
export function windowTitle(active: string | null): string {
  return active === null ? "agent-terminal" : `${displayName(active)} — agent-terminal`;
}

/** Grid floor for fit-to-window: below this a shell is unusable and
 * several TUIs misrender; a tiny window fits a small-but-sane grid. */
export const FIT_MIN_COLS = 20;
export const FIT_MIN_ROWS = 5;

/** The grid that fills a host of the given pixel size at the current
 * cell metrics — the number an explicit "fit session to window" action
 * sends as MSG_RESIZE. Floor, never round: a rounded-up column falls
 * outside the window and wraps every full-width line. Null when a
 * dimension is unmeasurable (unmounted, hidden, or zero-size cell) —
 * callers must skip the resize, not send a garbage grid. */
export function fitGrid(
  hostW: number,
  hostH: number,
  cellW: number,
  cellH: number,
): { cols: number; rows: number } | null {
  if (hostW <= 0 || hostH <= 0 || cellW <= 0 || cellH <= 0) return null;
  return {
    cols: Math.max(FIT_MIN_COLS, Math.floor(hostW / cellW)),
    rows: Math.max(FIT_MIN_ROWS, Math.floor(hostH / cellH)),
  };
}

/** Fraction of the host area the letterbox (dead space around the
 * terminal) occupies, 0..1. Unmeasurable input → 0: no reliable
 * number, no hint — a hint that appears from garbage measurements is
 * worse than none. */
export function letterboxFraction(
  hostW: number,
  hostH: number,
  termW: number,
  termH: number,
): number {
  if (hostW <= 0 || hostH <= 0 || termW <= 0 || termH <= 0) return 0;
  const used = Math.min(termW, hostW) * Math.min(termH, hostH);
  return 1 - used / (hostW * hostH);
}

/** True when the grid is bigger than the window in either axis, so the
 * view is clipped and has to be scrollable. Unmeasurable input → false,
 * the same rule as letterboxFraction: no reliable number, no claim.
 *
 * Not the complement of a large letterboxFraction — a wide-but-short grid
 * overflows sideways *and* leaves dead space below, so both would fire.
 * Only one hint can be shown, and the caller prefers this one: a clipped
 * session is the more urgent of the two and ⤢ answers both. */
export function overflowsHost(
  hostW: number,
  hostH: number,
  termW: number,
  termH: number,
): boolean {
  if (hostW <= 0 || hostH <= 0 || termW <= 0 || termH <= 0) return false;
  return termW > hostW || termH > hostH;
}

/** Scroll offset that puts the BOTTOM of a clipped grid on screen. That
 * is where a terminal's prompt and newest output live, so it is what the
 * default view has to show; scrolled to the top instead, a window one row
 * too short hides exactly the line the user is waiting for. Zero when the
 * grid fits — a small session keeps its dead space below it. */
export function bottomScrollTop(hostH: number, termH: number): number {
  if (hostH <= 0 || termH <= 0) return 0;
  return Math.max(0, Math.round(termH - hostH));
}

/** Show the in-letterbox fit hint only when the dead space is a real
 * fraction of the window — a sliver of margin is normal geometry, not
 * a problem worth labeling. */
export const FIT_HINT_MIN_FRACTION = 0.25;

/** One-shot name registry: consume(name) answers true exactly once per
 * add(name). Carries the "auto-fit exactly this newborn session, and
 * never again" rule — a second consume (re-selecting the session later)
 * must NOT re-impose geometry the user may have changed since. */
export function makeOneShotSet(): {
  add(name: string): void;
  consume(name: string): boolean;
} {
  const names = new Set<string>();
  return {
    add: (name) => void names.add(name),
    consume: (name) => names.delete(name),
  };
}
