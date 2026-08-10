// Control-plane API behind an interface, mirroring terminal/transport.ts:
// tests inject a mock; the real implementation invokes Tauri commands.

import { invoke } from "@tauri-apps/api/core";

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

export interface ControlApi {
  listSessions(): Promise<SessionRow[]>;
  newSession(name: string, argv: string[], cols: number, rows: number): Promise<void>;
  killSession(name: string): Promise<void>;
  listTemplates(): Promise<Template[]>;
}

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
