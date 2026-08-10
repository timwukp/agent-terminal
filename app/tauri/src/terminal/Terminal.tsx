// The terminal region: xterm.js fed verbatim from the session channel.
// Composited frames arrive pre-rendered from the daemon (dividers
// included) — there is deliberately no pane-rendering code here; panes
// exist client-side only as click targets from MSG_LAYOUT.

import { useEffect, useRef } from "react";
import { Terminal as XTerm } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";
import type { PaneRect, Transport } from "./transport";
import { paneAtPixel } from "./hittest";

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
    const fit = new FitAddon();
    term.loadAddon(fit);
    term.open(host);
    fit.fit();

    let disposed = false;

    const attach = async () => {
      await transport.attach(session, term.cols, term.rows, {
        onOutput: (bytes) => term.write(bytes),
        onSnapshot: (cols, rows, blob) => {
          // The blob addresses rows absolutely for exactly cols×rows;
          // resize the grid first so the repaint lands intact.
          if (term.cols !== cols || term.rows !== rows) term.resize(cols, rows);
          term.write(blob);
        },
        onCtrl: (ev) => {
          if (disposed) return;
          if (ev.kind === "layout") panesRef.current = ev.panes;
          else if (ev.kind === "closed") onClosed?.(ev.error);
          else if (ev.kind === "session_exited") onClosed?.(null);
        },
      });
    };
    void attach();

    const data = term.onData((s) => {
      void transport.stdin(new TextEncoder().encode(s));
    });

    // Click-to-focus: pixel → cell → pane id → SELECT_PANE mode 0.
    const onClick = (e: MouseEvent) => {
      if (panesRef.current.length < 2) return; // single pane: nothing to focus
      const rect = host.getBoundingClientRect();
      const core = (term as unknown as {
        _core: { _renderService: { dimensions: { css: { cell: { width: number; height: number } } } } };
      })._core;
      const cell = core._renderService.dimensions.css.cell;
      const id = paneAtPixel(
        e.clientX - rect.left,
        e.clientY - rect.top,
        { cellWidth: cell.width, cellHeight: cell.height },
        panesRef.current,
      );
      if (id !== null) void transport.selectPane(id);
    };
    host.addEventListener("click", onClick);

    const onResize = () => {
      fit.fit();
      void transport.resize(term.cols, term.rows);
    };
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
