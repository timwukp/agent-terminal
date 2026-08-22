// Usage-panel data source, behind an interface so tests inject fixtures
// (the Sidebar's ControlApi pattern). Shapes mirror claude-watch's
// serialization exactly — the Rust side is the source of truth.

import { invoke } from "@tauri-apps/api/core";
import { panelStream } from "./panelStream";

export interface Usage {
  input_tokens: number;
  output_tokens: number;
  cache_read_input_tokens: number;
  cache_creation_input_tokens: number;
}

export interface Bucket {
  minute: string; // "YYYY-MM-DDTHH:MM" (UTC)
  output_tokens: number;
}

export interface TranscriptUsage {
  id: string;
  project: string;
  totals: Usage;
  messages: number;
  malformed: number;
  model: string;
  last_timestamp: string;
  buckets: Bucket[];
  /** Bytes of this transcript not yet counted (the per-call read budget
   * ran out). Non-zero = the totals above are still climbing. */
  pending_bytes: number;
}

export interface UsageApi {
  /** One reading, for the first paint. */
  snapshot(): Promise<TranscriptUsage[]>;
  /** Subsequent readings, pushed when they differ (panelStream.ts).
   * Returns an unsubscribe. */
  subscribe(
    onData: (rows: TranscriptUsage[]) => void,
    onError: (msg: string) => void,
  ): () => void;
}

export class TauriUsageApi implements UsageApi {
  snapshot(): Promise<TranscriptUsage[]> {
    return invoke("usage_snapshot");
  }
  subscribe(
    onData: (rows: TranscriptUsage[]) => void,
    onError: (msg: string) => void,
  ): () => void {
    return panelStream.subscribe<TranscriptUsage[]>("usage", onData, onError);
  }
}

/** 1234 → "1.2k", 1234567 → "1.2M", 999 → "999". Token counts get big
 * fast (cache reads run to millions); the panel is 260px wide. */
export function fmtTokens(n: number): string {
  if (n >= 1_000_000) return `${(n / 1_000_000).toFixed(1)}M`;
  if (n >= 1_000) return `${(n / 1_000).toFixed(1)}k`;
  return String(n);
}

/** "3m ago" / "2h ago" / "just now" from an ISO timestamp. `nowMs` is a
 * parameter, not Date.now(), so tests pin it. Unparseable → "" (the
 * row simply shows no age rather than NaN). */
export function agoText(iso: string, nowMs: number): string {
  const t = Date.parse(iso);
  if (Number.isNaN(t)) return "";
  const s = Math.max(0, Math.floor((nowMs - t) / 1000));
  if (s < 60) return "just now";
  if (s < 3600) return `${Math.floor(s / 60)}m ago`;
  if (s < 86400) return `${Math.floor(s / 3600)}h ago`;
  return `${Math.floor(s / 86400)}d ago`;
}
