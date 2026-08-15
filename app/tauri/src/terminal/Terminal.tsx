// The terminal region: xterm.js fed verbatim from the session channel.
// Composited frames arrive pre-rendered from the daemon (dividers
// included) — there is deliberately no pane-rendering code here; panes
// exist client-side only as click targets from MSG_LAYOUT.

import { useEffect, useRef, useState } from "react";
import { Terminal as XTerm } from "@xterm/xterm";
import "@xterm/xterm/css/xterm.css";
import type { PaneRect, Transport } from "./transport";
import type { CellMetrics } from "./hittest";
import { paneAtPixel } from "./hittest";
import { createStdinQueue } from "./stdinQueue";
import { Backfill } from "./backfill";
import {
  FIT_HINT_MIN_FRACTION,
  FONT_DEFAULT,
  bottomScrollTop,
  fitGrid,
  isAtBottom,
  letterboxFraction,
  nextFontSize,
  overflowsHost,
  zoomActionForKey,
} from "./viewControls";
import { currentTheme, onThemeChange, resolveTokens, theme } from "../theme";
import { xtermTheme } from "./xtermTheme";
import { readCellMetrics } from "./overlay";
import { lastNonEmptyLine } from "./screenLine";
import { zoomedPaneId } from "./zoom";
import PaneOverlay from "./PaneOverlay";
import PaneToolbar from "./PaneToolbar";

interface LayoutState {
  panes: PaneRect[];
  activeId: number;
  viewCols: number;
  viewRows: number;
}

export interface TerminalProps {
  transport: Transport;
  session: string;
  onClosed?: (error: string | null) => void;
  /** A keystroke could not be delivered. Surfaced in the UI, because a
   * console message is invisible to the person typing — a build with a
   * stale frontend rejected every key and read as a product bug. */
  onStdinError?: (message: string) => void;
  /** Bumped by the host to demand keyboard focus. Clicking anything in
   * the sidebar moves DOM focus to that button, and a click on the
   * already-active session does not remount this component, so mount-time
   * focus alone leaves the user typing into a button. */
  focusNonce?: number;
  /** The session finished a turn: BEL rang (xterm's parser — a raw 0x07
   * scan would false-positive on every OSC title write), or the Rust idle
   * machine saw sustained work end. Carries the last non-empty screen
   * line so a notification can say WHAT finished. */
  onTurnDone?: (reason: "bell" | "idle", lastLine: string) => void;
  /** Consumed once after the first snapshot: true = fit the session to
   * this window now. App answers true only for a session THIS GUI just
   * created (we chose its 80×24 default; resizing our own newborn is
   * not imposing on anyone). CLI-created sessions always answer false —
   * the no-imposed-geometry rule stands. */
  autoFit?: () => boolean;
}

export default function TerminalView({
  transport,
  session,
  onClosed,
  onStdinError,
  focusNonce,
  onTurnDone,
  autoFit,
}: TerminalProps) {
  const hostRef = useRef<HTMLDivElement>(null);
  const panesRef = useRef<PaneRect[]>([]);
  const termRef = useRef<XTerm | null>(null);
  // Overlay inputs. The panes also live in panesRef because the click
  // handler is a DOM listener registered once — reading state there would
  // capture the mount-time value. State here exists to re-render the
  // overlay; the ref exists to be read from the closure. Both are set from
  // the same layout event, so they cannot disagree.
  const [layout, setLayout] = useState<LayoutState | null>(null);
  const [metrics, setMetrics] = useState<CellMetrics | null>(null);
  // How far a clipped view is scrolled inside its host, for the overlay.
  // Always {0,0} while the whole grid fits, which is the common case.
  const [pan, setPan] = useState({ x: 0, y: 0 });
  // The viewport is scrolled up: new output is arriving out of sight.
  const [behind, setBehind] = useState(false);
  // The window and the session's grid disagree, in one of two directions,
  // and either way ⤢ is the answer. "empty": a large letterbox around a
  // small session reads as "reserved space" (a real user asked what the
  // empty half was FOR — twice). "clipped": the grid is bigger than the
  // window, so part of the session is scrolled out of sight and the hint is
  // the only thing on screen that says so. Null = the two agree closely
  // enough to need no words.
  const [fitHint, setFitHint] = useState<{
    cols: number;
    rows: number;
    kind: "empty" | "clipped";
  } | null>(null);
  // Latest-ref like onTurnDoneRef below: the attach effect must not
  // depend on it, must not capture a stale one.
  const autoFitRef = useRef(autoFit);
  autoFitRef.current = autoFit;

  // The one sanctioned way the GUI changes a session's geometry: the
  // user pressed ⤢ / clicked the hint, or the session is our own
  // newborn (autoFit). Shared by all three entry points.
  const fitToWindow = () => {
    const term = termRef.current;
    const host = hostRef.current;
    const m = term ? readCellMetrics(term) : null;
    if (!term || !host || !m) return;
    const grid = fitGrid(host.clientWidth, host.clientHeight, m.cellWidth, m.cellHeight);
    if (grid && (grid.cols !== term.cols || grid.rows !== term.rows)) {
      void transport.resize(grid.cols, grid.rows);
    }
  };
  // Latest-ref: the attach effect must not depend on this callback (a
  // re-attach per render would drop and rebuild the connection), but it
  // must also not capture a stale one — the parent's handler closes over
  // mute state that changes.
  const onTurnDoneRef = useRef(onTurnDone);
  onTurnDoneRef.current = onTurnDone;

  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;

    // No linkHandler and no web-links addon, deliberately. Session output
    // is untrusted — it is whatever a program in the pane decided to print
    // — and an OSC 8 hyperlink lets that program choose both the visible
    // text and an unrelated destination. xterm's built-in fallback for an
    // activated link is `confirm()` then `window.open()`, and in THIS build
    // both are inert: wry 0.55.1's WKWebView UI delegate implements only
    // windowWillClose, runOpenPanel, requestMediaCapturePermission and
    // createWebViewWithConfiguration — no runJavaScriptConfirmPanel — so
    // WebKit completes `confirm()` with false and the click stops there.
    //
    // That is an accident of a dependency version, not a decision, which
    // is the reason this comment exists: adding the addon, setting a
    // linkHandler, or building against webkit2gtk (which does implement the
    // script dialogs) turns a line of session output into a navigation in
    // the user's browser. If links are ever wanted, they need an explicit
    // in-app confirmation showing the resolved URL — not the platform's.
    const term = new XTerm({
      // Sized to the daemon's own ring (SB_MEM_LINES_DEFAULT): attach
      // backfills up to that much history, and everything after attach
      // accumulates client-side, so the wheel scrolls back seamlessly
      // across the attach point.
      scrollback: 10000,
      fontFamily: "ui-monospace, Menlo, monospace",
      fontSize: FONT_DEFAULT,
      // Cells and chrome from the same tokens — xterm's canvas cannot
      // evaluate var(), so it gets the resolved hex (xtermTheme.ts).
      theme: xtermTheme(resolveTokens(), currentTheme()),
    });
    term.open(host);
    termRef.current = term;

    const themeSub = onThemeChange(() => {
      term.options.theme = xtermTheme(resolveTokens(), currentTheme());
    });

    // ⌘/Ctrl +/−/0: resize the glyphs, not the grid — the grid belongs
    // to the session. Swallow every event type of a matched chord;
    // acting on keydown only, or '=' leaks to the shell on keypress.
    term.attachCustomKeyEventHandler((ev) => {
      const action = zoomActionForKey(ev);
      if (action === null) return true;
      if (ev.type === "keydown") {
        const next = nextFontSize(term.options.fontSize ?? FONT_DEFAULT, action);
        if (next !== term.options.fontSize) {
          term.options.fontSize = next;
          // The renderer re-measures on its own schedule; the fit check
          // and overlay metrics must wait for the new cell size to exist.
          requestAnimationFrame(() => {
            fitView();
            setMetrics(readCellMetrics(term));
          });
        }
      }
      return false;
    });

    // "You are scrolled up" tracking. onScroll covers viewport moves;
    // onWriteParsed covers the buffer growing underneath a parked
    // viewport (xterm holds the view still, so no scroll event fires —
    // exactly the case the pill exists for).
    const updateBehind = () => {
      const buf = term.buffer.active;
      setBehind(!isAtBottom(buf.viewportY, buf.baseY));
    };
    const scrollEv = term.onScroll(updateBehind);
    const writeEv = term.onWriteParsed(updateBehind);

    // The grid is the session's, so it cannot be reflowed to the window —
    // that is the ⤢ action, and every attached viewer feels it. It must
    // not be visually SCALED to the window either, which is what this
    // function used to do: xterm turns a pointer position into a cell by
    // dividing a *visual* pixel offset (from element.getBoundingClientRect)
    // by its own *unscaled* cell width, so a scale multiplies the numerator
    // and never the denominator. Measured in Chromium at scale 0.6 with the
    // app's own xterm options: dragging across `bravo` selected "a b", the
    // native copy event carried "a b", and a click on cell (40,10) reported
    // (25,7) to the session — so vim or less in a letterboxed window got
    // the wrong cell, which is the severest of the three symptoms because
    // nothing on screen admits it. `transform: scale()` and CSS `zoom`
    // fail identically (both verified as applied, not ignored).
    //
    // So the view is always 1:1. A window smaller than the grid clips it
    // and scrolls, anchored to the bottom where the prompt is; the hint
    // names the mismatch and offers ⤢. ⌘− still shrinks the glyphs, which
    // makes a big grid fit while changing nothing on the far end.
    const fitView = () => {
      const el = term.element;
      if (!el) return;
      // Nothing sets these any more. They are cleared here because this is
      // where a future "just scale it to fit" edit would land, and the
      // invariant is asserted in fitView.test.tsx.
      el.style.transform = "";
      el.style.zoom = "";
      const w = el.offsetWidth;
      const h = el.offsetHeight;
      if (!w || !h) return;
      const hostW = host.clientWidth;
      const hostH = host.clientHeight;
      if (overflowsHost(hostW, hostH, w, h)) {
        host.scrollTop = bottomScrollTop(hostH, h);
        setFitHint({ cols: term.cols, rows: term.rows, kind: "clipped" });
      } else {
        // When the dead space around the terminal dominates, label it —
        // otherwise it reads as "reserved for something" (it is not).
        const frac = letterboxFraction(hostW, hostH, w, h);
        setFitHint(
          frac > FIT_HINT_MIN_FRACTION ? { cols: term.cols, rows: term.rows, kind: "empty" } : null,
        );
      }
      // The overlay is a sibling of the scrolling host (xterm owns the
      // host's DOM), so it has to be moved by the same offset or its boxes
      // drift off the panes they outline.
      setPan({ x: host.scrollLeft, y: host.scrollTop });
    };

    let disposed = false;

    // Mount-time focus is NOT taken here: the focusNonce effect below runs
    // on mount as well as on every bump, so a call here is dead weight.
    // Verified by mutation — deleting it failed nothing, which is the
    // signal that it was duplicated rather than that the test was weak.

    // Input is queued until ATTACH resolves; see stdinQueue.ts. Sending
    // before then hits "not attached" and the byte is gone.
    const stdin = createStdinQueue(
      (bytes) => transport.stdin(bytes),
      (e) => {
        console.error("stdin send failed", e);
        if (!disposed) onStdinError?.(String(e));
      },
    );

    // Attach-time history backfill (see backfill.ts for the ordering
    // story). The machine decides WHAT happens in which order; these
    // sinks are the only place that knows it happens to an xterm.
    const backfill = new Backfill({
      // Stored lines carry raw SGR and do not self-reset — bracket each
      // one exactly like the CLI pager does (pager.c pager_draw), or one
      // red line bleeds into every line after it.
      writeLine: (line) => {
        term.write("\x1b[0m");
        term.write(line);
        term.write("\x1b[0m\r\n");
      },
      padToScrollback: () => {
        // rows-1 newlines: enough to scroll the last written line off
        // the top from any cursor position, never one more (which would
        // put a spurious blank line into scrollback).
        term.write("\n".repeat(Math.max(0, term.rows - 1)));
      },
      applySnapshot: (_cols, _rows, blob) => {
        // Grid was already resized when the snapshot arrived (below);
        // here the stashed repaint finally lands.
        term.write(blob);
      },
      writeOutput: (bytes) => term.write(bytes),
      request: (startSeq, maxLines) => {
        // A failed request (detached mid-fetch) has no response frame;
        // the machine's stall timer converts that into paint-without-
        // history rather than a terminal that never renders.
        void transport.scrollbackReq(startSeq, maxLines).catch(() => backfill.onStall());
      },
    });

    // Auto-fit fires once, on the first snapshot (grid + cell metrics
    // are both real by then), and only if App says this session is our
    // newborn.
    let sawFirstSnapshot = false;

    const attach = async () => {
      // cols/rows 0 means "adopt the session's current size", verified
      // against the daemon: session_resize() early-returns on a zero
      // dimension (session.c:942) and the attach still answers with a
      // SNAPSHOT. Passing this window's size instead would reflow a live
      // session just because the GUI looked at it — measured: attaching
      // took the user's claude session from 111x54 to 93x48. The grid is
      // then sized from the snapshot in onSnapshot.
      await transport.attach(session, 0, 0, {
        onOutput: (bytes) => backfill.onOutput(bytes),
        onSnapshot: (cols, rows, sbLines, blob) => {
          // The blob addresses rows absolutely for exactly cols×rows;
          // resize the grid first so the repaint lands intact — and so
          // history lines written before the repaint wrap at the
          // session's width, not the mount-default 80. This is also how
          // the viewer learns the session's real geometry.
          if (term.cols !== cols || term.rows !== rows) term.resize(cols, rows);
          // Outside the resize branch on purpose: a session that happens to
          // be exactly the mount default (80×24) changes nothing here, and
          // gating the fit check on a size *change* left that session with
          // no hint at all until the user resized the window — the one case
          // where the hint is the only explanation on screen.
          fitView();
          backfill.onSnapshot(cols, rows, sbLines, blob);
          // Cell metrics change exactly when the grid or font does, and a
          // snapshot follows every geometry change — so this is the one
          // refresh point that cannot go stale.
          setMetrics(readCellMetrics(term));
          if (!sawFirstSnapshot) {
            sawFirstSnapshot = true;
            if (autoFitRef.current?.() === true) fitToWindow();
          }
        },
        onScrollback: (firstSeq, lines) => backfill.onScrollback(firstSeq, lines),
        onCtrl: (ev) => {
          if (disposed) return;
          if (ev.kind === "layout") {
            panesRef.current = ev.panes;
            setLayout({
              panes: ev.panes,
              activeId: ev.active_id,
              viewCols: ev.view_cols,
              viewRows: ev.view_rows,
            });
            // A layout can arrive before any snapshot has measured cells
            // (split of a session attached mid-life); measure here too.
            setMetrics(readCellMetrics(term));
          } else if (ev.kind === "turn_done") {
            onTurnDoneRef.current?.("idle", lastNonEmptyLine(term));
          } else if (ev.kind === "pane_bell") {
            // The split-session bell. xterm cannot ring it (composite frames
            // strip the raw \x07), so the daemon attributes it and we treat
            // it exactly like a local bell.
            onTurnDoneRef.current?.("bell", lastNonEmptyLine(term));
          } else if (ev.kind === "closed") onClosed?.(ev.error);
          else if (ev.kind === "session_exited") onClosed?.(null);
        },
      });
      if (!disposed) stdin.open();
    };
    void attach().catch((e) => {
      if (!disposed) onClosed?.(String(e));
    });

    const data = term.onData((s) => {
      stdin.push(new TextEncoder().encode(s));
      // Echo comes from the far end; keep the local view scrolled there.
      term.scrollToBottom();
    });

    // The single-pane bell trigger: raw output reaches xterm only when the
    // session is not composited. The split-session case arrives as the
    // pane_bell ctrl event above (MSG_PANE_BELL) — two disjoint paths, so a
    // bell can never ring twice.
    const bell = term.onBell(() => {
      if (!disposed) onTurnDoneRef.current?.("bell", lastNonEmptyLine(term));
    });

    // Click-to-focus: pixel → cell → pane id → SELECT_PANE mode 0.
    const onClick = (e: MouseEvent) => {
      // Keyboard focus first, unconditionally. A click that lands on the
      // letter-box margin rather than the terminal element is still the
      // user saying "type here", and xterm only self-focuses on a
      // mousedown inside its own element.
      term.focus();
      if (panesRef.current.length < 2) return; // single pane: no pane to select
      const el = term.element;
      if (!el) return;
      // Measure against the terminal element, not the host, and exactly as
      // xterm's own hit-test does: the rect is viewport-relative, so it
      // already carries any scroll of a clipped view, and the view is never
      // scaled — cell metrics are in the same CSS pixels as the rect. Those
      // two facts are what make this agree with xterm's selection; when the
      // element was scaled, they did not agree (see fitView).
      const rect = el.getBoundingClientRect();
      // Same guarded reader the overlay uses: one place touches xterm's
      // private metrics, and both consumers degrade the same way.
      const m = readCellMetrics(term);
      if (!m) return;
      const id = paneAtPixel(e.clientX - rect.left, e.clientY - rect.top, m, panesRef.current);
      if (id !== null) void transport.selectPane(id);
    };
    host.addEventListener("click", onClick);

    // Window resize deliberately does NOT send MSG_RESIZE: the session's
    // size belongs to the session (and to any CLI client sharing it), not
    // to this window. Growing the window shows the same grid larger, it
    // does not reflow the far end. An explicit "resize session to window"
    // action is the right home for that, and needs the observer-geometry
    // question settled first (app/design/deferred-daemon-work.md).
    const onResize = () => fitView();
    window.addEventListener("resize", onResize);

    // A clipped view can be panned (by the scrollbar, or by a wheel that
    // has run out of scrollback to consume). The overlay lives outside the
    // scroll container, so it only stays on its panes if it follows.
    const onPan = () => setPan({ x: host.scrollLeft, y: host.scrollTop });
    host.addEventListener("scroll", onPan);

    // Returning to the app must return the caret too. Switching away
    // (to a browser, to PowerPoint) and back otherwise leaves focus on
    // whatever last held it, which reads as a dead keyboard.
    const onWindowFocus = () => term.focus();
    window.addEventListener("focus", onWindowFocus);

    return () => {
      disposed = true;
      window.removeEventListener("focus", onWindowFocus);
      window.removeEventListener("resize", onResize);
      host.removeEventListener("scroll", onPan);
      host.removeEventListener("click", onClick);
      backfill.dispose();
      themeSub();
      scrollEv.dispose();
      writeEv.dispose();
      bell.dispose();
      data.dispose();
      void transport.detach();
      termRef.current = null;
      term.dispose();
    };
  }, [transport, session, onClosed, onStdinError]);

  // Take focus back whenever the host asks. Separate from the attach
  // effect on purpose: clicking the session you are already on must
  // refocus without tearing down a working attachment.
  useEffect(() => {
    termRef.current?.focus();
  }, [focusNonce]);

  const zoomed =
    layout !== null && zoomedPaneId(layout.panes, layout.viewCols, layout.viewRows) !== null;

  return (
    <div style={{ position: "relative", width: "100%", height: "100%" }}>
      {/* xterm owns everything inside this div; React must not render
        * children into it or the two will fight over the DOM. Overlay and
        * toolbar are siblings above it instead.
        *
        * overflow:auto, not hidden — the view is 1:1 (see fitView), so a
        * window smaller than the session's grid has to be pannable or the
        * rest of the session would be unreachable. `auto` shows nothing
        * when the grid fits, which is the ordinary case. */}
      <div ref={hostRef} style={{ position: "absolute", inset: 0, overflow: "auto" }} />
      <PaneOverlay
        panes={layout?.panes ?? []}
        activeId={layout?.activeId ?? 0}
        metrics={metrics}
        offsetX={pan.x}
        offsetY={pan.y}
      />
      <PaneToolbar
        onSplitVertical={() => void transport.splitPane(false)}
        onSplitHorizontal={() => void transport.splitPane(true)}
        onZoomToggle={() => void transport.zoomToggle()}
        onClose={() => void transport.closePane()}
        onFitToWindow={fitToWindow}
        paneCount={layout?.panes.length ?? 1}
        zoomed={zoomed}
      />
      {fitHint !== null && (
        <button
          onClick={fitToWindow}
          title={
            fitHint.kind === "clipped"
              ? "This window is smaller than the session's grid, so part of the session is scrolled out of view — scroll to reach it. The view is deliberately never shrunk to fit: a scaled terminal reports the wrong cell to the program running in it. Click to resize the session to this window instead (every attached viewer reflows), or press ⌘− to shrink the glyphs, which changes nothing on the far end."
              : "The dark area holds nothing — the session's grid is just smaller than the window. Click to resize the session to fill it (every attached viewer reflows)."
          }
          style={{
            position: "absolute",
            bottom: 48,
            left: "50%",
            transform: "translateX(-50%)",
            zIndex: 1,
            background: "transparent",
            color: theme.textMuted,
            border: `1px dashed ${theme.border}`,
            borderRadius: 6,
            padding: "4px 12px",
            fontSize: 11,
            cursor: "pointer",
          }}
        >
          {fitHint.kind === "clipped" ? "scroll to see it all" : "empty space"}{" "}
          — session is {fitHint.cols}×{fitHint.rows} · ⤢ fit to window
        </button>
      )}
      {behind && (
        <button
          onClick={() => {
            termRef.current?.scrollToBottom();
            termRef.current?.focus();
          }}
          title="Jump to the live bottom"
          style={{
            position: "absolute",
            bottom: 12,
            right: 16,
            zIndex: 2,
            background: theme.accent,
            color: theme.onAccent,
            border: "none",
            borderRadius: 12,
            padding: "3px 10px",
            fontSize: 12,
            cursor: "pointer",
            opacity: 0.9,
          }}
        >
          ↓ bottom
        </button>
      )}
    </div>
  );
}
