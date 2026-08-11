// Transport between the webview and the Rust core, behind an interface
// so tests can inject a mock without Tauri present.
//
// Channel payloads are tagged binary (src-tauri/src/session.rs):
//   0x30 raw output bytes                          → feed xterm verbatim
//   0x31 u16 cols, u16 rows, u64 sb_lines LE, blob → snapshot repaint
//   0x33 u64 first_seq, u32 nlines LE, {u32 len, bytes}… → history page
//   0x4a JSON control event                        → layout / err / exits / closed

import { Channel, invoke } from "@tauri-apps/api/core";

export interface PaneRect {
  id: number;
  x: number;
  y: number;
  cols: number;
  rows: number;
}

export type CtrlEvent =
  | { kind: "layout"; view_cols: number; view_rows: number; active_id: number; panes: PaneRect[] }
  | { kind: "err"; code: number; msg: string }
  | { kind: "session_exited"; exit_status: number }
  | { kind: "pane_exited"; pane_id: number; exit_status: number }
  // The Rust idle machine: sustained output followed by silence long
  // enough to call the turn finished (src-tauri/src/idle.rs).
  | { kind: "turn_done" }
  | { kind: "closed"; error: string | null };

export interface SessionEvents {
  onOutput(bytes: Uint8Array): void;
  /** sbLines = how many lines of history the daemon holds for this
   * session (used to size the backfill request; the blob itself is only
   * the visible screen). */
  onSnapshot(cols: number, rows: number, sbLines: number, blob: Uint8Array): void;
  /** One page of history lines (ANSI text, no trailing newlines),
   * answering a scrollbackReq. firstSeq is the seq of lines[0]. */
  onScrollback(firstSeq: number, lines: Uint8Array[]): void;
  onCtrl(ev: CtrlEvent): void;
}

export interface Transport {
  attach(session: string, cols: number, rows: number, events: SessionEvents): Promise<void>;
  stdin(bytes: Uint8Array): Promise<void>;
  resize(cols: number, rows: number): Promise<void>;
  selectPane(paneId: number): Promise<void>;
  /** Ask for up to maxLines of active-pane history starting at startSeq;
   * the page arrives via onScrollback. */
  scrollbackReq(startSeq: number, maxLines: number): Promise<void>;
  zoomToggle(): Promise<void>;
  splitPane(stacked: boolean): Promise<void>;
  closePane(): Promise<void>;
  detach(): Promise<void>;
}

/** Read a u64 LE as a JS number. Sequence numbers count lines, so they
 * sit far below 2^53; the high word is folded in rather than dropped so
 * a genuinely huge value fails loudly in tests instead of aliasing. */
function u64At(b: Uint8Array, off: number): number {
  const lo = (b[off] | (b[off + 1] << 8) | (b[off + 2] << 16)) + b[off + 3] * 0x1000000;
  const hi = (b[off + 4] | (b[off + 5] << 8) | (b[off + 6] << 16)) + b[off + 7] * 0x1000000;
  return hi * 0x100000000 + lo;
}

function u32At(b: Uint8Array, off: number): number {
  return (b[off] | (b[off + 1] << 8) | (b[off + 2] << 16)) + b[off + 3] * 0x1000000;
}

/** Route one tagged channel payload to the right handler. Exported for
 * tests: the tag byte and header parsing are the only wire logic that
 * lives on the JS side. */
export function routeMessage(data: Uint8Array, events: SessionEvents): void {
  if (data.length === 0) return;
  const tag = data[0];
  const body = data.subarray(1);
  if (tag === 0x30) {
    events.onOutput(body);
  } else if (tag === 0x31) {
    if (body.length < 12) return; // malformed; skip like unknown frames
    const cols = body[0] | (body[1] << 8);
    const rows = body[2] | (body[3] << 8);
    events.onSnapshot(cols, rows, u64At(body, 4), body.subarray(12));
  } else if (tag === 0x33) {
    if (body.length < 12) return;
    const firstSeq = u64At(body, 0);
    const nlines = u32At(body, 8);
    const lines: Uint8Array[] = [];
    let off = 12;
    for (let i = 0; i < nlines; i++) {
      if (off + 4 > body.length) return; // truncated: drop the whole page
      const len = u32At(body, off);
      if (off + 4 + len > body.length) return;
      lines.push(body.subarray(off + 4, off + 4 + len));
      off += 4 + len;
    }
    events.onScrollback(firstSeq, lines);
  } else if (tag === 0x4a) {
    events.onCtrl(JSON.parse(new TextDecoder().decode(body)) as CtrlEvent);
  }
  // Unknown tags: skipped by design, same rule as the wire protocol.
}

export class TauriTransport implements Transport {
  async attach(session: string, cols: number, rows: number, events: SessionEvents): Promise<void> {
    const chan = new Channel<ArrayBuffer | Uint8Array>();
    chan.onmessage = (msg) => {
      const bytes = msg instanceof Uint8Array ? msg : new Uint8Array(msg);
      routeMessage(bytes, events);
    };
    await invoke("attach_session", { session, cols, rows, chan });
  }
  stdin(bytes: Uint8Array): Promise<void> {
    // Raw ArrayBuffer, not Array.from(bytes): a number-array payload is
    // JSON-serialized per keystroke and arrives as InvokeBody::Json,
    // which the byte-slice command cannot read.
    return invoke("stdin_data", bytes);
  }
  resize(cols: number, rows: number): Promise<void> {
    return invoke("resize", { cols, rows });
  }
  selectPane(paneId: number): Promise<void> {
    return invoke("select_pane", { paneId });
  }
  scrollbackReq(startSeq: number, maxLines: number): Promise<void> {
    return invoke("scrollback_req", { startSeq, maxLines });
  }
  zoomToggle(): Promise<void> {
    return invoke("zoom_toggle");
  }
  splitPane(stacked: boolean): Promise<void> {
    return invoke("split_pane", { stacked });
  }
  closePane(): Promise<void> {
    return invoke("close_pane");
  }
  detach(): Promise<void> {
    return invoke("detach");
  }
}
