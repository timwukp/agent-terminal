// Security card (design: claude-panel.md + hook-log.md). Lives inside
// the Hooks tab — at 260px a third tab buys chrome, not clarity. Two
// halves: recent hook executions from the opt-in hash-chained log, and
// the chain verdict badge. The badge wording matches the design doc:
// tamper-EVIDENT, not tamper-proof — the honest claim is the feature.

import { useEffect, useState } from "react";
import type { HookLogSnapshot, HooksApi } from "./hooksApi";
import { theme } from "../theme";

export default function SecurityCard({ api }: { api: HooksApi }) {
  const [snap, setSnap] = useState<HookLogSnapshot | null>(null);

  // One read for the first paint, then pushed updates (panelStream.ts).
  // Errors are swallowed here on purpose: the rules table above shares
  // this api and surfaces them, and two alerts for one failure reads as
  // two failures.
  useEffect(() => {
    let live = true;
    api.logSnapshot().then(
      (s) => {
        if (live) setSnap(s);
      },
      () => {},
    );
    const off = api.subscribeLog(
      (s) => {
        if (live) setSnap(s);
      },
      () => {},
    );
    return () => {
      live = false;
      off();
    };
  }, [api]);

  if (snap === null) return null;

  return (
    <div style={{ marginTop: 10 }}>
      <h2 style={{ fontSize: 13, margin: "4px 0 6px", color: theme.text }}>Hook executions</h2>
      {!snap.exists ? (
        <p style={{ fontSize: 10, color: theme.textMuted }}>
          no hook log — hooks run silently. To record them (tamper-evident, opt-in), see
          app/design/hook-log.md.
        </p>
      ) : (
        <>
          <p
            style={{ fontSize: 10, color: snap.chain_ok ? theme.good : theme.danger }}
            title="plain SHA-256 chain: detects accidental damage and reordering — tamper-evident, not tamper-proof"
          >
            {snap.chain_ok
              ? `chain verified · ${snap.total} events`
              : `chain broken at line ${(snap.break_at ?? 0) + 1}`}
            {snap.malformed > 0 && ` · ${snap.malformed} unparsed`}
          </p>
          {snap.events.length === 0 && (
            <p style={{ fontSize: 10, color: theme.textMuted }}>log exists but holds no events</p>
          )}
          <ul style={{ listStyle: "none", padding: 0, margin: 0 }}>
            {[...snap.events].reverse().map((e, i) => (
              <li
                key={`${e.ts}#${i}`}
                title={`${e.event} → ${e.hook}: ${e.reason}`}
                style={{ fontSize: 10, color: theme.textMuted, padding: "1px 0" }}
              >
                <span
                  style={{
                    color: e.decision === "block" ? theme.danger : theme.good,
                  }}
                >
                  ●
                </span>{" "}
                <span style={{ color: theme.text }}>{e.decision || "?"}</span> {e.tool} ·{" "}
                {e.hook} <span title={e.ts}>{e.ts.slice(11, 19)}</span>
              </li>
            ))}
          </ul>
        </>
      )}
    </div>
  );
}
