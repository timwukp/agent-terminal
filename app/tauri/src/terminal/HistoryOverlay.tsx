// The deep-history viewer: a read-only second terminal over the live one,
// driven by the HistoryView machine (historyView.ts, which owns every
// decision this file wires up).
//
// A second XTerm rather than a hand-rolled text view because the lines the
// daemon serves are RENDERED ANSI — the same bytes the CLI's copy-mode hands
// to a terminal (pager.c) — so anything less than a VT emulator shows escape
// sequences instead of colour. It is created when the viewer opens and
// disposed when it closes, which is what keeps the window's ~6 MB of cell
// storage from being a permanent cost of the feature.
//
// Read-only is enforced twice: `disableStdin` stops xterm's textarea from
// accepting input, and nothing here wires onData to the transport. The keys
// this view claims move the WINDOW or the viewport — there is no cursor here
// to move — and they are handled on the container, so they cannot reach the
// live session behind it.

import { useEffect, useRef, useState, type KeyboardEvent as ReactKeyboardEvent } from "react";
import { Terminal as XTerm } from "@xterm/xterm";
import { currentTheme, onThemeChange, resolveTokens, theme } from "../theme";
import { xtermTheme } from "./xtermTheme";
import { HistoryView, type HistoryState, WINDOW_LINES } from "./historyView";

export interface HistoryOverlayProps {
  /** Lines of history the session has (the snapshot's sb_lines). A later
   * snapshot may raise it; the window stays where the user put it. */
  total: number;
  /** Where to open. The caller passes the oldest line the live terminal
   * holds, so this window ENDS where the live buffer begins. Read once, at
   * mount: it is an opening position, not a binding. */
  startSeq: number;
  /** The session's grid, so history wraps as it did when it was written —
   * and at 1:1, the same rule the live view follows. */
  cols: number;
  rows: number;
  /** Ask the daemon for a page (transport.scrollbackReq). */
  request(startSeq: number, maxLines: number): void;
  /** Hand the machine to the host, which routes MSG_SCROLLBACK_DATA to
   * whichever of Backfill / HistoryView is waiting for one. Called with
   * null on unmount, so a page can never reach a disposed machine. */
  register(view: HistoryView | null): void;
  onClose(): void;
}

/** "1,001–5,000 of 93,374" — a position the user can compare against
 * `agent-terminal history | wc -l`, not a percentage. An empty window (the
 * log ends shallower than the snapshot claimed) says so rather than
 * rendering "1–0". */
export function positionLabel(s: HistoryState): string {
  if (s.loaded === 0) return s.loading ? "loading…" : `no lines here · ${fmt(s.total)} total`;
  return `${fmt(s.anchor + 1)}–${fmt(s.anchor + s.loaded)} of ${fmt(s.total)}`;
}

function fmt(n: number): string {
  return n.toLocaleString("en-US");
}

/** The buffer rows logical history line `index` occupies, as `[start, end]`.
 *
 * The machine counts stored lines; xterm scrolls to rows, and the two are the
 * same number only while nothing wraps. A stored line is one screen row as
 * wide as the session's grid WAS when it scrolled off (`vt_cb_scrollback` in
 * session.c pushes `cols` cells), so history written at 155 columns wraps in a
 * viewer opened at 80 — which this GUI causes routinely, since ⤢ resizes the
 * session. Anchored on the raw index, opening the viewer then lands one row
 * short per wrapped line: the join with the live buffer is off screen and the
 * user has to hunt for it.
 *
 * xterm has already parsed the wrapping, so ask it (`isWrapped`) instead of
 * re-deriving widths from the bytes — those carry SGR, so counting cells there
 * means a second VT parser. Rows before the first line and after the last are
 * unwrapped too, but the scan stops at `index + 1`, so trailing blank viewport
 * rows are never mistaken for content. */
export function rowSpanOfLine(
  buf: { length: number; getLine(y: number): { isWrapped: boolean } | undefined },
  index: number,
): { start: number; end: number } {
  let logical = -1;
  let start = 0;
  for (let row = 0; row < buf.length; row++) {
    if (buf.getLine(row)?.isWrapped === true) continue;
    logical++;
    if (logical === index) start = row;
    else if (logical > index) return { start, end: row - 1 };
  }
  // The line asked for is the last one in the buffer (or, if the buffer is
  // somehow shorter than the machine believes, the closest row that exists).
  return { start, end: Math.max(start, buf.length - 1) };
}

/** Same style as the pane toolbar's buttons, so the one validated
 * text-on-raised contrast pair covers both (theme.test.ts). */
const btnStyle: React.CSSProperties = {
  background: theme.raised,
  color: theme.text,
  border: `1px solid ${theme.raisedBorder}`,
  borderRadius: 4,
  cursor: "pointer",
  fontSize: 12,
  lineHeight: "18px",
  padding: "1px 7px",
};

export default function HistoryOverlay({
  total,
  startSeq,
  cols,
  rows,
  request,
  register,
  onClose,
}: HistoryOverlayProps) {
  const rootRef = useRef<HTMLDivElement>(null);
  const hostRef = useRef<HTMLDivElement>(null);
  const termRef = useRef<XTerm | null>(null);
  const viewRef = useRef<HistoryView | null>(null);
  const [state, setState] = useState<HistoryState | null>(null);

  // Latest-refs: the mount effect must run exactly once (rebuilding the
  // terminal would throw away the user's position), so everything it closes
  // over is read through a ref that the render keeps current.
  const propsRef = useRef({ request, register, onClose });
  propsRef.current = { request, register, onClose };
  const openRef = useRef({ startSeq, total });

  // True only while the viewport belongs to the USER. Everything below reads
  // an edge as "page the window", and xterm reaches both edges on its own:
  //
  //  - `reset()` fires one onScroll at [0, 0], and a re-anchor resets before
  //    it asks for anything, so the machine's `loading` guard is not yet
  //    armed. Unguarded, `earlier()` re-enters the seek that is still
  //    running and the window walks itself back to seq 0 — measured, 34
  //    nested seeks and 54 page requests from one open() on a 93,374-line
  //    log.
  //  - a page's writes are parsed on xterm's own queue, so their scroll
  //    events (viewport pinned to the bottom) arrive AFTER the last page
  //    cleared `loading`, and `open()` deliberately parks the viewport at the
  //    bottom edge — the join. Unguarded, `later()` fires the instant the
  //    viewer finishes opening and pages forward off the join.
  //
  // Cleared before the reset that starts a move, set again inside the write
  // callback that lands the machine's own scroll — after which every scroll
  // event is one the user caused. One flag rather than a guard per edge: the
  // question both edges are really asking is "did the user do this?".
  const settled = useRef(false);

  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;
    const { startSeq: from, total: announced } = openRef.current;
    const term = new XTerm({
      // One window plus a screen: the machine never writes more than
      // WINDOW_LINES, and the headroom keeps xterm from evicting the top of
      // the window while its last page is still arriving.
      scrollback: WINDOW_LINES + rows,
      fontFamily: "ui-monospace, Menlo, monospace",
      fontSize: 13,
      // A document, not a session: no input, no cursor to mislead.
      disableStdin: true,
      cursorBlink: false,
      theme: xtermTheme(resolveTokens(), currentTheme()),
      cols,
      rows,
    });
    term.open(host);
    termRef.current = term;
    const themeSub = onThemeChange(() => {
      term.options.theme = xtermTheme(resolveTokens(), currentTheme());
    });

    const view = new HistoryView(
      {
        reset: () => {
          settled.current = false;
          term.reset();
        },
        // Same bracketing as the live backfill and the CLI pager: stored
        // lines carry raw SGR and do not self-reset, so one red line would
        // otherwise bleed into every line below it.
        writeLine: (line) => {
          term.write("\x1b[0m");
          term.write(line);
          term.write("\x1b[0m\r\n");
        },
        request: (s, max) => propsRef.current.request(s, max),
        scrollTo: (line, where) => {
          // xterm parses writes on its own queue, so a scroll issued now
          // would run against a buffer that does not hold those lines yet.
          // A trailing write with a callback is the supported way to act
          // after the queue drains.
          term.write("\x1b[0m", () => {
            // Logical line → buffer row, because a wrapped line is more than
            // one row (rowSpanOfLine). "top" wants the row the line starts on;
            // "bottom" wants the row it ends on, at the foot of the viewport.
            const span = rowSpanOfLine(term.buffer.active, line);
            term.scrollToLine(
              where === "top" ? span.start : Math.max(0, span.end - term.rows + 1),
            );
            // From here the viewport is the user's. After the scroll rather
            // than before it only so the two lines read in the order they
            // happen — NOT as a guard: no reachable move lands on an edge
            // this handler would act on. open() settles one row above the
            // bottom (viewportY 3970, baseY 3971, measured), because the
            // cursor's own trailing row is below the window's last line; ⤒
            // and ⤓ do land on edges, and there the machine already refuses
            // to page (anchor <= 0, anchor + loaded >= total). So there is
            // nothing here for a test to kill, and none is claimed.
            settled.current = true;
          });
        },
        onState: (s) => setState(s),
      },
      announced,
    );
    viewRef.current = view;
    propsRef.current.register(view);

    // Wheel and scrollbar paging: reaching an edge moves the window. This is
    // the gesture the feature exists for — the user scrolls up and keeps
    // scrolling — so it must not require finding a button. Only the user's
    // own scrolls count (see `settled`); after a move the viewport lands
    // mid-window, so an edge cannot re-trigger itself from there either.
    const scrollEv = term.onScroll(() => {
      if (!settled.current) return;
      const buf = term.buffer.active;
      if (buf.viewportY <= 0) view.earlier();
      else if (buf.viewportY >= buf.baseY) view.later();
    });

    view.open(from);
    // Take focus, or the keys below reach the live session behind this view.
    // Via a ref on the element that carries tabIndex and onKeyDown, not by
    // walking up from the xterm host: a DOM walk is silently wrong the moment
    // a wrapper is added, and its failure is keystrokes reaching the session.
    rootRef.current?.focus();

    return () => {
      propsRef.current.register(null);
      viewRef.current = null;
      termRef.current = null;
      view.dispose();
      scrollEv.dispose();
      themeSub();
      term.dispose();
    };
  }, []);

  // The session was resized while the viewer was open. Reflow rather than
  // rebuild: xterm rewraps its buffer, so the window keeps its content and
  // the user keeps their place.
  useEffect(() => {
    const term = termRef.current;
    if (term && (term.cols !== cols || term.rows !== rows)) term.resize(cols, rows);
  }, [cols, rows]);

  // A new snapshot announced more history. Only the ceiling moves.
  useEffect(() => {
    viewRef.current?.setTotal(total);
  }, [total]);

  const onKeyDown = (e: ReactKeyboardEvent) => {
    const view = viewRef.current;
    const term = termRef.current;
    if (!view || !term) return;
    // Every key this view understands is one the session must not see.
    switch (e.key) {
      case "Escape":
        propsRef.current.onClose();
        break;
      case "Home":
        view.toOldest();
        break;
      case "End":
        view.toNewest();
        break;
      case "ArrowUp":
        term.scrollLines(-1);
        break;
      case "ArrowDown":
        term.scrollLines(1);
        break;
      case "PageUp":
        term.scrollPages(-1);
        break;
      case "PageDown":
        term.scrollPages(1);
        break;
      default:
        return; // not ours: leave it alone (⌘W, ⌘Q, …)
    }
    // Scrolling to an edge is what moves the window; that is the onScroll
    // handler's job, not this one's.
    e.preventDefault();
    e.stopPropagation();
  };

  const btn = (label: string, title: string, disabled: boolean, onClick: () => void) => (
    <button
      onClick={onClick}
      disabled={disabled}
      title={title}
      style={{ ...btnStyle, ...(disabled ? { cursor: "default", opacity: 0.45 } : null) }}
    >
      {label}
    </button>
  );

  const atOldest = state?.canEarlier !== true;
  const atNewest = state?.canLater !== true;
  return (
    <div
      ref={rootRef}
      // tabIndex so the container holds focus and receives the keys; the
      // xterm inside has no input of its own to compete for them.
      tabIndex={0}
      onKeyDown={onKeyDown}
      data-testid="history-overlay"
      style={{
        position: "absolute",
        inset: 0,
        // Above the pane overlay and the pane toolbar: while history is open
        // those act on a session this view is not showing.
        zIndex: 3,
        display: "flex",
        flexDirection: "column",
        background: theme.bgMain,
        outline: "none",
      }}
    >
      <div
        style={{
          display: "flex",
          alignItems: "center",
          gap: 8,
          padding: "4px 8px",
          borderBottom: `1px solid ${theme.border}`,
          background: theme.surface,
          fontSize: 11,
          color: theme.textMuted,
          flex: "0 0 auto",
        }}
      >
        <span style={{ color: theme.text }}>history</span>
        <span data-testid="history-position">{state ? positionLabel(state) : "loading…"}</span>
        {state?.stalled === true && (
          <button
            onClick={() => viewRef.current?.retry()}
            style={{ ...btnStyle, color: theme.danger }}
            title="The daemon did not answer the last page. Nothing is lost — the lines already here are correct."
          >
            stalled — retry
          </button>
        )}
        <span style={{ flex: 1 }} />
        {btn("⤒", "Oldest line in the log (Home)", atOldest, () => viewRef.current?.toOldest())}
        {btn("↑", "Half a window earlier", atOldest, () => viewRef.current?.earlier())}
        {btn("↓", "Half a window later", atNewest, () => viewRef.current?.later())}
        {btn("⤓", "Newest history, where the live view begins (End)", atNewest, () =>
          viewRef.current?.toNewest(),
        )}
        {btn("✕", "Close and return to the live session (Esc)", false, () =>
          propsRef.current.onClose(),
        )}
      </div>
      {/* 1:1 like the live view, for the same reason: the grid is the
       * session's, and a scaled terminal misreports cells. A narrower
       * window clips and scrolls rather than shrinking the glyphs. */}
      <div ref={hostRef} style={{ flex: 1, minHeight: 0, overflow: "auto" }} />
    </div>
  );
}
