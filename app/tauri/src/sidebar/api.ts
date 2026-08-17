// Control-plane API behind an interface, mirroring terminal/transport.ts:
// tests inject a mock; the real implementation invokes Tauri commands.

import { Channel, invoke } from "@tauri-apps/api/core";

export interface SessionRow {
  name: string;
  view_cols: number;
  view_rows: number;
  alive: boolean;
  nclients: number;
  pid: number;
  npanes: number | null;
  zoomed: boolean | null;
}

export interface Template {
  label: string;
  name_prefix: string;
  argv: string[];
}

/** Callbacks for the daemon's session-table push (MSG_SESSIONS_CHANGED). */
export interface SessionWatch {
  /** Whether push is available right now. While `true` the caller may
   * stop polling; `false` means it must poll — and it is delivered
   * explicitly, on every transition, because "no frames" is also what a
   * healthy idle daemon looks like. */
  onPush(live: boolean): void;
  /** The session table changed: re-list. Carries no data — neither does
   * the daemon's message. */
  onChanged(): void;
}

export interface ControlApi {
  listSessions(): Promise<SessionRow[]>;
  newSession(name: string, argv: string[], cols: number, rows: number): Promise<void>;
  killSession(name: string): Promise<void>;
  listTemplates(): Promise<Template[]>;
  /** Subscribe to session-table pushes; returns an unsubscribe.
   *
   * Optional so that "no watch at all" stays a first-class path rather
   * than an untested fallback: every existing test's api object omits it
   * and therefore exercises polling, which is still what runs against a
   * daemon too old to push. */
  watchSessions?(w: SessionWatch): () => void;
}

/** One frame from the `session_watch` channel (src-tauri/src/control.rs
 * WatchFrame — the shapes are pinned by a test on that side). */
type WatchFrame = { kind: "sessions_push"; data: { live: boolean } } | { kind: "sessions_changed" };

export class TauriControlApi implements ControlApi {
  listSessions(): Promise<SessionRow[]> {
    return invoke("list_sessions");
  }
  newSession(name: string, argv: string[], cols: number, rows: number): Promise<void> {
    return invoke("new_session", { name, argv, cols, rows });
  }
  killSession(name: string): Promise<void> {
    return invoke("kill_session", { name });
  }
  listTemplates(): Promise<Template[]> {
    return invoke("list_templates");
  }

  watchSessions(w: SessionWatch): () => void {
    let cancelled = false;
    // `new Channel()` reaches into window.__TAURI_INTERNALS__ synchronously
    // and throws outside a webview, so it is built inside the promise: as a
    // rejection this degrades to polling, as a throw it would take out the
    // effect that subscribed (same reason as TauriStreamHost.open).
    const started = (async () => {
      const chan = new Channel<WatchFrame>();
      chan.onmessage = (f) => {
        if (cancelled || f === null || typeof f !== "object") return;
        // An unknown kind is ignored, matching the wire protocol's rule for
        // unknown message types — a newer backend must not break this one.
        if (f.kind === "sessions_push") w.onPush(f.data.live === true);
        else if (f.kind === "sessions_changed") w.onChanged();
      };
      return invoke<number>("session_watch", { chan });
    })();
    // A watcher that could not start must say so: silence would read as
    // "push is live and nothing has changed", and the sidebar would sit
    // there not polling either.
    started.catch(() => {
      if (!cancelled) w.onPush(false);
    });
    return () => {
      cancelled = true;
      // The id only exists once the open resolves; a failed open has
      // nothing to stop.
      void started.then(
        (id) => void invoke("session_watch_stop", { id }),
        () => {},
      );
    };
  }
}

/** Pick a free session name from a template prefix: the bare prefix if
 * unused, else prefix-2, prefix-3, … (matching how humans number
 * things; never reuses a live name). Exported for tests. */
export function nextSessionName(prefix: string, existing: string[]): string {
  const taken = new Set(existing);
  if (!taken.has(prefix)) return prefix;
  for (let i = 2; ; i++) {
    const candidate = `${prefix}-${i}`;
    if (!taken.has(candidate)) return candidate;
  }
}
