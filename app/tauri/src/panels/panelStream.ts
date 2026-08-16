// One push stream for the filesystem-backed panels, replacing the three
// setInterval(2000) pollers the Usage panel, Hooks panel and security
// card each ran (design: app/design/claude-panel.md). Rust polls once and
// sends a frame only when a snapshot changed; see src-tauri/src/panels.rs
// for why it is still a timer over there and not a filesystem watcher.
//
// The sidebar keeps polling. Its data is a daemon round-trip and
// MSG_SESSION_LIST exists only as a REPLY to MSG_LIST_SESSIONS
// (src/common/proto.h) — there is no unsolicited push to ride, so moving
// it needs a new protocol message, not a new frontend module.
//
// The stream lives behind a StreamHost so tests drive it without mocking
// Tauri, the same reason UsageApi/HooksApi/ControlApi are interfaces.

import { Channel, invoke } from "@tauri-apps/api/core";

export type PanelKind = "usage" | "hooks" | "hook_log";

export interface PanelFrame {
  kind: PanelKind;
  data: unknown;
}

/** Fixed order, so the same set of subscribers always produces the same
 * `kinds` argument and an unchanged interest is recognizable as one. */
const ORDER: readonly PanelKind[] = ["usage", "hooks", "hook_log"];

export interface StreamHost {
  /** Start a stream for `kinds`, resolving to its id. Frames arrive on
   * `onFrame` until the stream is replaced or closed. Failure is a
   * rejection; a synchronous throw is handled the same way, but is not
   * part of the contract. */
  open(kinds: readonly PanelKind[], onFrame: (frame: PanelFrame) => void): Promise<number>;
  /** Stop the stream with `id`. Naming the id matters: this and `open`
   * are separate IPC calls with no ordering guarantee, so an
   * unconditional stop can land after the next open. */
  close(id: number): void;
}

export class TauriStreamHost implements StreamHost {
  // `async` on purpose: `new Channel()` reaches into
  // window.__TAURI_INTERNALS__ synchronously, which throws outside a
  // webview (jsdom, a browser-served dev build). As a rejection that
  // becomes the panel's alert row; as a throw it would take out the
  // effect that called it.
  async open(
    kinds: readonly PanelKind[],
    onFrame: (frame: PanelFrame) => void,
  ): Promise<number> {
    const chan = new Channel<PanelFrame>();
    chan.onmessage = onFrame;
    return invoke<number>("panel_stream", { kinds, chan });
  }
  close(id: number): void {
    void invoke("panel_stream_stop", { id });
  }
}

interface Sub {
  kind: PanelKind;
  onData: (data: unknown) => void;
  onError: (msg: string) => void;
}

export class PanelStream {
  private subs = new Set<Sub>();
  /** The kinds the running stream was opened for; "" when none is. */
  private opened = "";
  /** Bumped on every (re)open so a superseded stream's late frames and a
   * superseded open's rejection are both ignorable. */
  private gen = 0;
  private start: Promise<number> | null = null;

  constructor(private readonly host: StreamHost) {}

  /** Receive `kind`'s snapshots as they change. `onError` fires when the
   * stream itself cannot be established — a panel that silently stops
   * updating is indistinguishable from one where nothing is happening. */
  subscribe<T>(
    kind: PanelKind,
    onData: (data: T) => void,
    onError: (msg: string) => void,
  ): () => void {
    const sub: Sub = { kind, onData: onData as (data: unknown) => void, onError };
    this.subs.add(sub);
    this.resync();
    return () => {
      this.subs.delete(sub);
      this.resync();
    };
  }

  /** Open, replace, or close the stream so it matches current interest.
   * A second subscriber to an already-streamed kind changes nothing —
   * that is what keeps a tab switch from restarting the stream twice. */
  private resync(): void {
    const kinds = ORDER.filter((k) => this.wants(k));
    const sig = kinds.join(",");
    if (sig === this.opened) return;
    this.opened = sig;
    const previous = this.start;
    const gen = ++this.gen;

    if (kinds.length === 0) {
      this.start = null;
      // Closing waits for the open to resolve because the id is what
      // comes back from it; a failed open has no stream to close.
      if (previous !== null) {
        void previous.then(
          (id) => this.host.close(id),
          () => {},
        );
      }
      return;
    }

    // A host that throws instead of rejecting still has to end up on the
    // failure path: leaving `opened` set with no stream behind it would
    // wedge the panel until the tab was switched.
    let start: Promise<number>;
    try {
      start = this.host.open(kinds, (frame) => {
        if (gen === this.gen) this.dispatch(frame);
      });
    } catch (e: unknown) {
      start = Promise.reject(e instanceof Error ? e : new Error(String(e)));
    }
    this.start = start;
    start.catch((e: unknown) => {
      if (gen !== this.gen) return;
      const msg = String(e);
      for (const sub of this.subs) sub.onError(msg);
    });
  }

  private wants(kind: PanelKind): boolean {
    for (const sub of this.subs) if (sub.kind === kind) return true;
    return false;
  }

  /** Fan one frame out to that kind's subscribers. A frame for a kind
   * nobody holds — the tail of a stream being replaced — reaches nobody,
   * which is the same rule the wire protocol uses for unknown tags. */
  private dispatch(frame: PanelFrame): void {
    if (frame === null || typeof frame !== "object") return;
    for (const sub of this.subs) if (sub.kind === frame.kind) sub.onData(frame.data);
  }
}

/** The window's stream. One per window: the Rust side keeps one thread
 * per window and replaces it, so a second instance here would fight it
 * for ownership. */
export const panelStream = new PanelStream(new TauriStreamHost());
