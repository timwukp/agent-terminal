// Hooks tab: Claude Code's hook rules, read-only (claude-panel.md — the
// GUI never writes settings.json; concurrent writes with Claude Code
// are a corruption hazard). Grouped by event, one row per command;
// clicking a row shows the script source verbatim, because heuristic
// summaries are opinions and the script is the truth.

import { useEffect, useState } from "react";
import type { HooksApi, HooksSnapshot } from "./hooksApi";
import { commandBasename, isScriptPath } from "./hooksApi";
import { theme } from "../theme";

const POLL_MS = 2000;

interface Viewer {
  command: string;
  /** null while loading; the command string itself for inline commands. */
  source: string | null;
  error: string | null;
}

export default function HooksPanel({ api }: { api: HooksApi }) {
  const [snap, setSnap] = useState<HooksSnapshot | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [viewer, setViewer] = useState<Viewer | null>(null);

  useEffect(() => {
    let live = true;
    const poll = () =>
      api.snapshot().then(
        (s) => {
          if (live) {
            setSnap(s);
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

  const open = (command: string) => {
    if (!isScriptPath(command)) {
      // Inline command: the command string IS the source.
      setViewer({ command, source: command, error: null });
      return;
    }
    setViewer({ command, source: null, error: null });
    api.readScript(command).then(
      (src) => setViewer((v) => (v?.command === command ? { ...v, source: src } : v)),
      (e) => setViewer((v) => (v?.command === command ? { ...v, error: String(e) } : v)),
    );
  };

  const events = [...new Set((snap?.rules ?? []).map((r) => r.event))];

  return (
    <div>
      <h2 style={{ fontSize: 13, margin: "4px 0 8px", color: theme.text }}>Hooks</h2>
      {error !== null && (
        <p role="alert" style={{ fontSize: 11, color: theme.danger }}>
          {error}
        </p>
      )}
      {snap !== null && !snap.exists && (
        <p style={{ fontSize: 11, color: theme.textMuted }}>no {snap.path} found</p>
      )}
      {snap !== null && snap.exists && snap.rules.length === 0 && snap.malformed === 0 && (
        <p style={{ fontSize: 11, color: theme.textMuted }}>no hooks configured</p>
      )}
      {snap !== null && snap.malformed > 0 && (
        <p
          style={{ fontSize: 10, color: theme.danger }}
          title="entries the parser could not read — the table may be incomplete"
        >
          {snap.malformed} unparsed
        </p>
      )}
      {events.map((event) => (
        <div key={event} style={{ marginBottom: 8 }}>
          <div style={{ fontSize: 11, color: theme.textMuted, margin: "4px 0 2px" }}>{event}</div>
          {(snap?.rules ?? [])
            .filter((r) => r.event === event)
            .map((r, i) => (
              <button
                key={`${r.command}#${i}`}
                onClick={() => open(r.command)}
                title={`${r.command} — click to view source`}
                style={{
                  display: "block",
                  width: "100%",
                  textAlign: "left",
                  background: theme.surface,
                  border: `1px solid ${theme.border}`,
                  borderRadius: 6,
                  padding: "5px 8px",
                  marginBottom: 4,
                  cursor: "pointer",
                  color: theme.text,
                  fontSize: 11,
                }}
              >
                <span style={{ color: theme.textMuted }}>{r.matcher}</span>{" "}
                <span style={{ fontFamily: "ui-monospace, Menlo, monospace" }}>
                  {commandBasename(r.command)}
                </span>
                {r.timeout !== null && (
                  <span style={{ color: theme.textMuted }}> · {r.timeout}s</span>
                )}
              </button>
            ))}
        </div>
      ))}
      {viewer !== null && (
        <div
          style={{
            background: theme.surface,
            border: `1px solid ${theme.border}`,
            borderRadius: 6,
            padding: 8,
            marginTop: 4,
          }}
        >
          <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 4 }}>
            <span style={{ fontSize: 10, color: theme.textMuted }}>
              {commandBasename(viewer.command)} — read-only
            </span>
            <button
              onClick={() => setViewer(null)}
              aria-label="close source viewer"
              style={{
                border: "none",
                background: "transparent",
                color: theme.textMuted,
                cursor: "pointer",
                fontSize: 11,
              }}
            >
              ✕
            </button>
          </div>
          {viewer.error !== null ? (
            <p role="alert" style={{ fontSize: 10, color: theme.danger }}>
              {viewer.error}
            </p>
          ) : viewer.source === null ? (
            <p style={{ fontSize: 10, color: theme.textMuted }}>loading…</p>
          ) : (
            <pre
              style={{
                margin: 0,
                fontSize: 10,
                color: theme.text,
                whiteSpace: "pre-wrap",
                wordBreak: "break-all",
                maxHeight: 240,
                overflowY: "auto",
              }}
            >
              {viewer.source}
            </pre>
          )}
        </div>
      )}
    </div>
  );
}
