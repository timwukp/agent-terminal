// Token usage panel (design: app/design/claude-panel.md; data:
// claude-watch via usage_snapshot). Shows the transcripts active in the
// last 48 h, newest first: totals as text rows (the accessible "table
// view"), plus one micro sparkline of output tokens/min for the newest
// transcript. Single series → no legend; the caption names it.
//
// Correlation to a specific terminal session is deliberately NOT
// guessed at (two claude instances in one cwd are indistinguishable) —
// the panel presents transcripts as what they are, per the design
// doc's "aggregate with a badge, never a wrong guess" rule.

import { useEffect, useState } from "react";
import type { Bucket, TranscriptUsage, UsageApi } from "./usageApi";
import { agoText, fmtTokens } from "./usageApi";
import { theme } from "../theme";

const POLL_MS = 2000;

/** Inline SVG micro bars: baseline-anchored, 2px gaps, value scaled to
 * the window max. Native title per bar is the hover layer at this size. */
export function Sparkline({ buckets }: { buckets: Bucket[] }) {
  const W = 228;
  const H = 28;
  const slot = W / Math.max(buckets.length, 1);
  const gap = 2;
  const max = Math.max(...buckets.map((b) => b.output_tokens), 1);
  return (
    <svg width={W} height={H} role="img" aria-label="output tokens per minute">
      {buckets.map((b, i) => {
        const h = Math.max(b.output_tokens > 0 ? 2 : 0, (b.output_tokens / max) * H);
        return (
          <rect
            key={b.minute}
            x={i * slot}
            y={H - h}
            width={Math.max(slot - gap, 1)}
            height={h}
            rx={1}
            fill={theme.accent}
          >
            <title>{`${b.minute}Z — ${b.output_tokens.toLocaleString()} out tokens`}</title>
          </rect>
        );
      })}
    </svg>
  );
}

const num: React.CSSProperties = {
  fontVariantNumeric: "tabular-nums",
  color: theme.text,
};

function Counter({ label, value }: { label: string; value: number }) {
  return (
    <span style={{ marginRight: 10, fontSize: 11, color: theme.textMuted }}>
      {label} <span style={num}>{fmtTokens(value)}</span>
    </span>
  );
}

export default function TokenPanel({ api }: { api: UsageApi }) {
  const [rows, setRows] = useState<TranscriptUsage[] | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let live = true;
    const poll = () =>
      api.snapshot().then(
        (r) => {
          if (live) {
            setRows(r);
            setError(null);
          }
        },
        (e) => {
          if (live) setError(String(e));
        },
      );
    void poll();
    const t = setInterval(() => void poll(), POLL_MS);
    return () => {
      live = false;
      clearInterval(t);
    };
  }, [api]);

  return (
    <div>
      <h2 style={{ fontSize: 13, margin: "4px 0 8px", color: theme.text }}>Token usage</h2>
      {error !== null && (
        <p role="alert" style={{ fontSize: 11, color: theme.danger }}>
          {error}
        </p>
      )}
      {rows !== null && rows.length === 0 && (
        <p style={{ fontSize: 11, color: theme.textMuted }}>
          no transcripts active in the last 48 h — claude writes them under ~/.claude/projects as
          it runs
        </p>
      )}
      {(rows ?? []).slice(0, 6).map((r, i) => (
        <div
          key={`${r.project}/${r.id}`}
          style={{
            background: theme.surface,
            border: `1px solid ${theme.border}`,
            borderRadius: 6,
            padding: "6px 8px",
            marginBottom: 6,
          }}
        >
          <div style={{ display: "flex", justifyContent: "space-between", alignItems: "baseline" }}>
            {/* File stem, not a session name: binding to a terminal
              * session is not guessed (claude-panel.md). */}
            <span style={{ fontSize: 11, color: theme.text, fontFamily: "ui-monospace, Menlo, monospace" }}>
              {r.id.slice(0, 8)}
            </span>
            <span style={{ fontSize: 10, color: theme.textMuted }} title={r.last_timestamp}>
              {agoText(r.last_timestamp, Date.now())}
            </span>
          </div>
          <div style={{ fontSize: 10, color: theme.textMuted, marginBottom: 2 }}>
            {r.model} · {r.messages} msgs
            {r.malformed > 0 && (
              <span
                title="lines the parser could not read — totals undercount by these"
                style={{ color: theme.danger }}
              >
                {" "}
                · {r.malformed} unparsed
              </span>
            )}
          </div>
          <div>
            <Counter label="in" value={r.totals.input_tokens} />
            <Counter label="out" value={r.totals.output_tokens} />
            <Counter label="cache r" value={r.totals.cache_read_input_tokens} />
            <Counter label="cache w" value={r.totals.cache_creation_input_tokens} />
          </div>
          {i === 0 && r.buckets.length > 0 && (
            <div style={{ marginTop: 4 }}>
              <Sparkline buckets={r.buckets} />
              <div style={{ fontSize: 9, color: theme.textMuted }}>out tokens/min · last 30 min</div>
            </div>
          )}
        </div>
      ))}
    </div>
  );
}
