// Session dashboard: LIST_SESSIONS2 polled every 2 s, click to switch,
// template buttons, right-click kill (design: ux-spec.md).

import { useCallback, useEffect, useState } from "react";
import type { ControlApi, SessionRow, Template } from "./api";
import { nextSessionName } from "./api";

const POLL_MS = 2000;

export interface SidebarProps {
  api: ControlApi;
  active: string | null;
  onSelect(name: string): void;
}

export default function Sidebar({ api, active, onSelect }: SidebarProps) {
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
          <li key={s.name}>
            <button
              onClick={() => onSelect(s.name)}
              onContextMenu={(e) => {
                e.preventDefault();
                void kill(s.name);
              }}
              style={{
                display: "block",
                width: "100%",
                textAlign: "left",
                padding: "6px 8px",
                border: "none",
                borderRadius: 4,
                background: s.name === active ? "#2b6cb0" : "transparent",
                color: s.name === active ? "#fff" : "inherit",
                cursor: "pointer",
                fontSize: 13,
              }}
              title={`${s.view_cols}x${s.view_rows}, pid ${s.pid}, ${s.nclients} client(s)`}
            >
              {s.name}
              {s.npanes != null && s.npanes > 1 && (
                <span style={{ opacity: 0.75 }}> {s.npanes}⧉</span>
              )}
              {s.zoomed === true && <span style={{ opacity: 0.75 }}> 🔍</span>}
              {s.nclients > 0 && (
                <span style={{ float: "right", opacity: 0.6, fontSize: 11 }}>{s.nclients}●</span>
              )}
            </button>
          </li>
        ))}
        {sessions.length === 0 && !error && (
          <li style={{ fontSize: 12, color: "#888", padding: 8 }}>no sessions</li>
        )}
      </ul>
      {error && (
        <p style={{ fontSize: 11, color: "#c33", margin: "4px 0" }} role="alert">
          {error}
        </p>
      )}
      <div style={{ borderTop: "1px solid #ccc", paddingTop: 6 }}>
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
            }}
          >
            + {t.label}
          </button>
        ))}
      </div>
    </div>
  );
}
