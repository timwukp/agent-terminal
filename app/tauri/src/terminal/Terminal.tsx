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
import { FONT_DEFAULT, isAtBottom, nextFontSize, zoomActionForKey } from "./viewControls";
import { readCellMetrics } from "./overlay";
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
}

/** Bottom-most non-empty row of the visible screen. */
function lastNonEmptyLine(term: XTerm): string {
  const buf = term.buffer.active;
  for (let y = buf.length - 1; y >= 0; y--) {
    const text = buf.getLine(y)?.translateToString(true).trim();
    if (text) return text;
  }
  return "";
}

export default function TerminalView({
  transport,
  session,
  onClosed,
  onStdinError,
  focusNonce,
  onTurnDone,
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
  const [scale, setScale] = useState(1);
  // The viewport is scrolled up: new output is arriving out of sight.
  const [behind, setBehind] = useState(false);
  // Latest-ref: the attach effect must not depend on this callback (a
  // re-attach per render would drop and rebuild the connection), but it
  // must also not capture a stale one — the parent's handler closes over
  // mute state that changes.
  const onTurnDoneRef = useRef(onTurnDone);
  onTurnDoneRef.current = onTurnDone;

  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;

    const term = new XTerm({
      // Sized to the daemon's own ring (SB_MEM_LINES_DEFAULT): attach
      // backfills up to that much history, and everything after attach
      // accumulates client-side, so the wheel scrolls back seamlessly
      // across the attach point.
      scrollback: 10000,
      fontFamily: "ui-monospace, Menlo, monospace",
      fontSize: FONT_DEFAULT,
    });
    term.open(host);
    termRef.current = term;

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
          // The renderer re-measures on its own schedule; scale and
          // overlay metrics must wait for the new cell size to exist.
          requestAnimationFrame(() => {
            scaleToFit();
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

    // The grid is the session's, so it cannot be reflowed to the window.
    // Instead the rendered terminal is scaled to fit inside it, letter-
    // boxed, preserving aspect ratio and never scaling past 1:1 (upscaled
    // text is blurry; a smaller session simply sits centred).
    const scaleToFit = () => {
      const el = term.element;
      if (!el) return;
      el.style.transformOrigin = "top left";
      el.style.transform = "";
      const w = el.offsetWidth;
      const h = el.offsetHeight;
      if (!w || !h) return;
      const k = Math.min(host.clientWidth / w, host.clientHeight / h, 1);
      el.style.transform = `scale(${k})`;
      // The overlay must carry the same transform or its boxes drift off
      // the panes they outline exactly when the window is small — the
      // case where a user is most likely to be squinting at panes.
      setScale(k);
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
          if (term.cols !== cols || term.rows !== rows) {
            term.resize(cols, rows);
            scaleToFit();
          }
          backfill.onSnapshot(cols, rows, sbLines, blob);
          // Cell metrics change exactly when the grid or font does, and a
          // snapshot follows every geometry change — so this is the one
          // refresh point that cannot go stale.
          setMetrics(readCellMetrics(term));
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

    // The bell trigger. Reliable single-pane only: composited multi-pane
    // frames rebuild from grid state and drop BEL (protocol-notes.md
    // trap #1) — the idle machine covers that case until MSG_PANE_BELL.
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
      // Measure against the terminal element, not the host: it is the
      // scaled box, so its client rect already carries the scale factor
      // and cell sizes stay in unscaled CSS units.
      const rect = el.getBoundingClientRect();
      const k = rect.width / el.offsetWidth || 1;
      // Same guarded reader the overlay uses: one place touches xterm's
      // private metrics, and both consumers degrade the same way.
      const m = readCellMetrics(term);
      if (!m) return;
      const id = paneAtPixel((e.clientX - rect.left) / k, (e.clientY - rect.top) / k, m, panesRef.current);
      if (id !== null) void transport.selectPane(id);
    };
    host.addEventListener("click", onClick);

    // Window resize deliberately does NOT send MSG_RESIZE: the session's
    // size belongs to the session (and to any CLI client sharing it), not
    // to this window. Growing the window shows the same grid larger, it
    // does not reflow the far end. An explicit "resize session to window"
    // action is the right home for that, and needs the observer-geometry
    // question settled first (app/design/deferred-daemon-work.md).
    const onResize = () => scaleToFit();
    window.addEventListener("resize", onResize);

    // Returning to the app must return the caret too. Switching away
    // (to a browser, to PowerPoint) and back otherwise leaves focus on
    // whatever last held it, which reads as a dead keyboard.
    const onWindowFocus = () => term.focus();
    window.addEventListener("focus", onWindowFocus);

    return () => {
      disposed = true;
      window.removeEventListener("focus", onWindowFocus);
      window.removeEventListener("resize", onResize);
      host.removeEventListener("click", onClick);
      backfill.dispose();
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
        * toolbar are siblings above it instead. */}
      <div ref={hostRef} style={{ position: "absolute", inset: 0 }} />
      <PaneOverlay
        panes={layout?.panes ?? []}
        activeId={layout?.activeId ?? 0}
        metrics={metrics}
        scale={scale}
      />
      <PaneToolbar
        onSplitVertical={() => void transport.splitPane(false)}
        onSplitHorizontal={() => void transport.splitPane(true)}
        onZoomToggle={() => void transport.zoomToggle()}
        onClose={() => void transport.closePane()}
        paneCount={layout?.panes.length ?? 1}
        zoomed={zoomed}
      />
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
            background: "#2b6cb0",
            color: "#fff",
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
