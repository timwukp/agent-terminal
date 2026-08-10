// The terminal region: xterm.js fed verbatim from the session channel.
// Composited frames arrive pre-rendered from the daemon (dividers
// included) — there is deliberately no pane-rendering code here; panes
// exist client-side only as click targets from MSG_LAYOUT.

import { useEffect, useRef } from "react";
import { Terminal as XTerm } from "@xterm/xterm";
import "@xterm/xterm/css/xterm.css";
import type { PaneRect, Transport } from "./transport";
import { paneAtPixel } from "./hittest";
import { createStdinQueue } from "./stdinQueue";

export interface TerminalProps {
  transport: Transport;
  session: string;
  onClosed?: (error: string | null) => void;
}

export default function TerminalView({ transport, session, onClosed }: TerminalProps) {
  const hostRef = useRef<HTMLDivElement>(null);
  const panesRef = useRef<PaneRect[]>([]);

  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;

    const term = new XTerm({
      scrollback: 0, // history lives daemon-side; copy-mode/scrollback UI later
      fontFamily: "ui-monospace, Menlo, monospace",
      fontSize: 13,
    });
    term.open(host);

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
    };

    let disposed = false;

    // Keyboard focus: xterm only focuses itself on a mousedown inside
    // the terminal element, so without this the window opens with input
    // going nowhere — keys are typed and simply vanish.
    term.focus();

    // Input is queued until ATTACH resolves; see stdinQueue.ts. Sending
    // before then hits "not attached" and the byte is gone.
    const stdin = createStdinQueue(
      (bytes) => transport.stdin(bytes),
      (e) => console.error("stdin send failed", e),
    );

    const attach = async () => {
      // cols/rows 0 means "adopt the session's current size", verified
      // against the daemon: session_resize() early-returns on a zero
      // dimension (session.c:942) and the attach still answers with a
      // SNAPSHOT. Passing this window's size instead would reflow a live
      // session just because the GUI looked at it — measured: attaching
      // took the user's claude session from 111x54 to 93x48. The grid is
      // then sized from the snapshot in onSnapshot.
      await transport.attach(session, 0, 0, {
        onOutput: (bytes) => term.write(bytes),
        onSnapshot: (cols, rows, blob) => {
          // The blob addresses rows absolutely for exactly cols×rows;
          // resize the grid first so the repaint lands intact. This is
          // also how the viewer learns the session's real geometry.
          if (term.cols !== cols || term.rows !== rows) {
            term.resize(cols, rows);
            scaleToFit();
          }
          term.write(blob);
        },
        onCtrl: (ev) => {
          if (disposed) return;
          if (ev.kind === "layout") panesRef.current = ev.panes;
          else if (ev.kind === "closed") onClosed?.(ev.error);
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

    // Click-to-focus: pixel → cell → pane id → SELECT_PANE mode 0.
    const onClick = (e: MouseEvent) => {
      if (panesRef.current.length < 2) return; // single pane: nothing to focus
      const el = term.element;
      if (!el) return;
      // Measure against the terminal element, not the host: it is the
      // scaled box, so its client rect already carries the scale factor
      // and cell sizes stay in unscaled CSS units.
      const rect = el.getBoundingClientRect();
      const k = rect.width / el.offsetWidth || 1;
      const core = (term as unknown as {
        _core: { _renderService: { dimensions: { css: { cell: { width: number; height: number } } } } };
      })._core;
      const cell = core._renderService.dimensions.css.cell;
      const id = paneAtPixel(
        (e.clientX - rect.left) / k,
        (e.clientY - rect.top) / k,
        { cellWidth: cell.width, cellHeight: cell.height },
        panesRef.current,
      );
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

    return () => {
      disposed = true;
      window.removeEventListener("resize", onResize);
      host.removeEventListener("click", onClick);
      data.dispose();
      void transport.detach();
      term.dispose();
    };
  }, [transport, session, onClosed]);

  return <div ref={hostRef} style={{ width: "100%", height: "100%" }} />;
}
