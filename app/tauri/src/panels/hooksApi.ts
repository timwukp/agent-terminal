// Hooks-panel data source behind an interface (the usageApi pattern:
// tests inject fixtures). Shapes mirror src-tauri/src/hooks.rs.

import { invoke } from "@tauri-apps/api/core";

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
  snapshot(): Promise<HooksSnapshot>;
  /** Source of a configured hook script. Rejects for inline commands
   * and for anything not currently in the snapshot (hooks.rs gate). */
  readScript(command: string): Promise<string>;
  /** Recent hook executions + chain verdict (app/design/hook-log.md). */
  logSnapshot(): Promise<HookLogSnapshot>;
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
