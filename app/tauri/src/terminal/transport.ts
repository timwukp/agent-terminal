// Transport between the webview and the Rust core, behind an interface
// so tests can inject a mock without Tauri present.
//
// Channel payloads are tagged binary (src-tauri/src/session.rs):
//   0x30 raw output bytes            → feed xterm verbatim
//   0x31 u16 cols, u16 rows LE, blob → resize-aware snapshot repaint
//   0x4a JSON control event          → layout / err / exits / closed

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
  | { kind: "closed"; error: string | null };

export interface SessionEvents {
  onOutput(bytes: Uint8Array): void;
  onSnapshot(cols: number, rows: number, blob: Uint8Array): void;
  onCtrl(ev: CtrlEvent): void;
}

export interface Transport {
  attach(session: string, cols: number, rows: number, events: SessionEvents): Promise<void>;
  stdin(bytes: Uint8Array): Promise<void>;
  resize(cols: number, rows: number): Promise<void>;
  selectPane(paneId: number): Promise<void>;
  zoomToggle(): Promise<void>;
  splitPane(stacked: boolean): Promise<void>;
  closePane(): Promise<void>;
  detach(): Promise<void>;
}

/** Route one tagged channel payload to the right handler. Exported for
 * tests: the tag byte and snapshot header parsing are the only wire
 * logic that lives on the JS side. */
export function routeMessage(data: Uint8Array, events: SessionEvents): void {
  if (data.length === 0) return;
  const tag = data[0];
  const body = data.subarray(1);
  if (tag === 0x30) {
    events.onOutput(body);
  } else if (tag === 0x31) {
    if (body.length < 4) return; // malformed; skip like unknown frames
    const cols = body[0] | (body[1] << 8);
    const rows = body[2] | (body[3] << 8);
    events.onSnapshot(cols, rows, body.subarray(4));
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
    return invoke("stdin_data", { bytes: Array.from(bytes) });
  }
  resize(cols: number, rows: number): Promise<void> {
    return invoke("resize", { cols, rows });
  }
  selectPane(paneId: number): Promise<void> {
    return invoke("select_pane", { paneId });
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
