// Session dashboard: LIST_SESSIONS2 polled every 2 s, click to switch,
// template buttons, right-click kill (design: ux-spec.md).

import { useCallback, useEffect, useState } from "react";
import type { ControlApi, SessionRow, Template } from "./api";
import { nextSessionName } from "./api";
import { theme } from "../theme";

const POLL_MS = 2000;

export interface SidebarProps {
  api: ControlApi;
  active: string | null;
  onSelect(name: string): void;
  /** Sessions whose completion notifications are silenced. */
  muted?: ReadonlySet<string>;
  /** Sessions that finished a turn while the window was unfocused; the
   * mark is the fallback when OS notifications cannot be delivered, and
   * it survives a mute — muting silences the pop-up, not the record. */
  done?: ReadonlySet<string>;
  /** Toggle notification mute for one session. A row button rather than
   * the design doc's context menu: right-click is already kill, and a
   * two-item menu for one toggle is more chrome than the toggle. */
  onToggleMute?(name: string): void;
}

export default function Sidebar({ api, active, onSelect, muted, done, onToggleMute }: SidebarProps) {
  const [sessions, setSessions] = useState<SessionRow[]>([]);
  const [templates, setTemplates] = useState<Template[]>([]);
  const [error, setError] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    try {
      setSessions(await api.listSessions());
      setError(null);
    } catch (e) {
      setError(String(e));
    }
  }, [api]);

  useEffect(() => {
    void refresh();
    const t = setInterval(() => void refresh(), POLL_MS);
    return () => clearInterval(t);
  }, [refresh]);

  useEffect(() => {
    api.listTemplates().then(setTemplates, (e) => setError(String(e)));
  }, [api]);

  const create = async (tpl: Template) => {
    const name = nextSessionName(
      tpl.name_prefix,
      sessions.map((s) => s.name),
    );
    try {
      await api.newSession(name, tpl.argv, 80, 24);
      await refresh();
      onSelect(name);
    } catch (e) {
      setError(String(e));
    }
  };

  const kill = async (name: string) => {
    if (!window.confirm(`Kill session "${name}"? Its child process ends.`)) return;
    try {
      await api.killSession(name);
      await refresh();
    } catch (e) {
      setError(String(e));
    }
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", height: "100%" }}>
      <h2 style={{ fontSize: 14, margin: "4px 0" }}>Sessions</h2>
      <ul style={{ listStyle: "none", padding: 0, margin: 0, flex: 1, overflowY: "auto" }}>
        {sessions.map((s) => (
          // Two sibling buttons, not a control nested inside a control: a
          // button inside a button is invalid HTML, double-announces to
          // screen readers, and needs a stopPropagation hack to keep the
          // mute click from also attaching.
          <li key={s.name} style={{ display: "flex", alignItems: "center" }}>
            <button
              onClick={() => onSelect(s.name)}
              onContextMenu={(e) => {
                e.preventDefault();
                void kill(s.name);
              }}
              style={{
                flex: 1,
                minWidth: 0,
                textAlign: "left",
                padding: "6px 8px",
                border: "none",
                borderRadius: 4,
                background: s.name === active ? theme.accent : "transparent",
                color: s.name === active ? "#fff" : "inherit",
                cursor: "pointer",
                fontSize: 13,
              }}
              title={`${s.view_cols}x${s.view_rows}, pid ${s.pid}, ${s.nclients} client(s) — right-click kills (asks first)`}
            >
              {s.name}
              {done?.has(s.name) === true && (
                <span title="finished while you were away" style={{ color: theme.good }}>
                  {" "}
                  ✓
                </span>
              )}
              {s.npanes != null && s.npanes > 1 && (
                <span style={{ opacity: 0.75 }}> {s.npanes}⧉</span>
              )}
              {s.zoomed === true && <span style={{ opacity: 0.75 }}> 🔍</span>}
              {s.nclients > 0 && (
                <span style={{ float: "right", opacity: 0.6, fontSize: 11 }}>{s.nclients}●</span>
              )}
            </button>
            {onToggleMute && (
              <button
                aria-label={
                  muted?.has(s.name) === true
                    ? `unmute notifications for ${s.name}`
                    : `mute notifications for ${s.name}`
                }
                aria-pressed={muted?.has(s.name) === true}
                title={muted?.has(s.name) === true ? "notifications muted" : "mute notifications"}
                onClick={() => onToggleMute(s.name)}
                style={{
                  border: "none",
                  background: "transparent",
                  opacity: muted?.has(s.name) === true ? 0.9 : 0.35,
                  fontSize: 11,
                  padding: "2px 4px",
                  cursor: "pointer",
                }}
              >
                {muted?.has(s.name) === true ? "🔕" : "🔔"}
              </button>
            )}
          </li>
        ))}
        {sessions.length === 0 && !error && (
          <li style={{ fontSize: 12, color: theme.textMuted, padding: 8 }}>no sessions</li>
        )}
      </ul>
      {error && (
        <p style={{ fontSize: 11, color: theme.danger, margin: "4px 0" }} role="alert">
          {error}
        </p>
      )}
      <div style={{ borderTop: `1px solid ${theme.border}`, paddingTop: 6 }}>
        {templates.map((t) => (
          <button
            key={t.label}
            onClick={() => void create(t)}
            style={{
              display: "block",
              width: "100%",
              margin: "3px 0",
              padding: "6px 8px",
              fontSize: 12,
              cursor: "pointer",
              background: theme.surface,
              color: theme.text,
              border: `1px solid ${theme.border}`,
              borderRadius: 4,
            }}
          >
            + {t.label}
          </button>
        ))}
      </div>
    </div>
  );
}
