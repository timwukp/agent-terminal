// Hooks-panel data source behind an interface (the usageApi pattern:
// tests inject fixtures). Shapes mirror src-tauri/src/hooks.rs.

import { invoke } from "@tauri-apps/api/core";
import { panelStream } from "./panelStream";

export interface HookRule {
  event: string;
  matcher: string;
  command: string;
  timeout: number | null;
}

export interface HooksSnapshot {
  path: string;
  exists: boolean;
  rules: HookRule[];
  malformed: number;
}

export interface HookLogEvent {
  ts: string;
  hook: string;
  event: string;
  tool: string;
  decision: string;
  reason: string;
}

export interface HookLogSnapshot {
  path: string;
  exists: boolean;
  events: HookLogEvent[];
  total: number;
  malformed: number;
  chain_ok: boolean;
  break_at: number | null;
}

export interface HooksApi {
  /** One reading, for the first paint. */
  snapshot(): Promise<HooksSnapshot>;
  /** Source of a configured hook script. Rejects for inline commands
   * and for anything not currently in the snapshot (hooks.rs gate). */
  readScript(command: string): Promise<string>;
  /** Recent hook executions + chain verdict (app/design/hook-log.md). */
  logSnapshot(): Promise<HookLogSnapshot>;
  /** Subsequent rule readings, pushed when they differ (panelStream.ts).
   * Returns an unsubscribe. */
  subscribe(onData: (snap: HooksSnapshot) => void, onError: (msg: string) => void): () => void;
  /** The same, for the hook log the security card shows. */
  subscribeLog(
    onData: (snap: HookLogSnapshot) => void,
    onError: (msg: string) => void,
  ): () => void;
}

export class TauriHooksApi implements HooksApi {
  snapshot(): Promise<HooksSnapshot> {
    return invoke("hooks_snapshot");
  }
  readScript(command: string): Promise<string> {
    return invoke("read_hook_script", { command });
  }
  logSnapshot(): Promise<HookLogSnapshot> {
    return invoke("hook_log_snapshot");
  }
  subscribe(onData: (snap: HooksSnapshot) => void, onError: (msg: string) => void): () => void {
    return panelStream.subscribe<HooksSnapshot>("hooks", onData, onError);
  }
  subscribeLog(
    onData: (snap: HookLogSnapshot) => void,
    onError: (msg: string) => void,
  ): () => void {
    return panelStream.subscribe<HookLogSnapshot>("hook_log", onData, onError);
  }
}

/** Last path segment for the table row; the full command lives in the
 * row tooltip and the viewer. */
export function commandBasename(command: string): string {
  const seg = command.split("/").filter(Boolean);
  return seg.length > 0 ? seg[seg.length - 1] : command;
}

/** Heuristic: a command that is a bare absolute path is a script file
 * we can open; anything with spaces/flags is an inline command and the
 * command string itself is the source. */
export function isScriptPath(command: string): boolean {
  return command.startsWith("/") && !command.includes(" ");
}
