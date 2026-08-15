// Session dashboard: LIST_SESSIONS2 polled every 2 s, click to switch,
// template buttons, right-click kill (design: ux-spec.md).

import { useCallback, useEffect, useRef, useState } from "react";
import type { ControlApi, SessionRow, Template } from "./api";
import { nextSessionName } from "./api";
import { displayName } from "../displayName";
import { theme } from "../theme";

const POLL_MS = 2000;

export interface SidebarProps {
  api: ControlApi;
  active: string | null;
  onSelect(name: string): void;
  /** A session THIS sidebar just created (before onSelect fires). The
   * app uses it to fit newborns to the window once. */
  onCreated?(name: string): void;
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

export default function Sidebar({
  api,
  active,
  onSelect,
  onCreated,
  muted,
  done,
  onToggleMute,
}: SidebarProps) {
  const [sessions, setSessions] = useState<SessionRow[]>([]);
  const [templates, setTemplates] = useState<Template[]>([]);
  const [error, setError] = useState<string | null>(null);
  // The kill confirmation, in-app. `window.confirm` cannot be used here:
  // wry 0.55.1's WKWebView UI delegate implements no
  // runJavaScriptConfirmPanel, so on macOS the call completes FALSE without
  // showing anything — the guard did not ask, it silently refused, and the
  // kill button was dead in every packaged build (Terminal.tsx says more).
  // The pid is remembered with the name because the daemon addresses
  // sessions by name and a freed name is reused by nextSessionName: without
  // it, confirming a stale prompt could kill a DIFFERENT session that
  // arrived at the same name.
  const [pending, setPending] = useState<{ name: string; pid: number } | null>(null);
  const cancelRef = useRef<HTMLButtonElement | null>(null);

  const refresh = useCallback(async () => {
    try {
      const rows = await api.listSessions();
      setSessions(rows);
      // A prompt outlives its subject: the child can exit while the
      // question is on screen, and the name can then come back attached to
      // a new pid. Either way the answer would no longer be about what was
      // asked, so the prompt goes rather than the target being re-bound.
      setPending((p) => (p == null || rows.some((r) => r.name === p.name && r.pid === p.pid) ? p : null));
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
      onCreated?.(name);
      await refresh();
      onSelect(name);
    } catch (e) {
      setError(String(e));
    }
  };

  // Focus lands on Cancel, not on Kill: for a destructive prompt the key
  // someone is already holding down must not be the one that confirms it.
  useEffect(() => {
    if (pending != null) cancelRef.current?.focus();
  }, [pending]);

  const kill = async (name: string) => {
    setPending(null);
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
        {sessions.map((s) => {
          // Everything a person READS goes through displayName; everything
          // that ADDRESSES a session — the key, onSelect, setPending, kill,
          // the mute and done lookups, the active comparison — carries
          // `s.name` unchanged. The daemon knows a session by its exact
          // bytes, so a row that renders one string and acts on another is
          // how a session becomes unkillable from its own row.
          const shown = displayName(s.name);
          return (
            // Two sibling buttons, not a control nested inside a control: a
            // button inside a button is invalid HTML, double-announces to
            // screen readers, and needs a stopPropagation hack to keep the
            // mute click from also attaching.
            <li key={s.name} style={{ display: "flex", alignItems: "center" }}>
              {pending?.name === s.name ? (
                // The question replaces the row it is about, so the name on
                // screen is the name that will be killed — a prompt floating
                // elsewhere can be read against the wrong row.
                <div
                  role="group"
                  aria-label={`confirm killing session ${shown}`}
                  onKeyDown={(e) => {
                    if (e.key === "Escape") setPending(null);
                  }}
                  style={{
                    flex: 1,
                    minWidth: 0,
                    padding: "6px 8px",
                    borderRadius: 4,
                    border: `1px solid ${theme.danger}`,
                    fontSize: 12,
                  }}
                >
                  {/* The pid is shown here, not only in the row tooltip, and
                      it is the answer to the one spoof no character rule can
                      catch: `deploy` and `dеploy` with a Cyrillic е are both
                      well-formed names that render identically, so the name
                      alone cannot tell you which session you are about to
                      end. A number can. It is the same pid the prompt was
                      opened with — refresh() drops a prompt whose row no
                      longer matches on name AND pid. */}
                  <div style={{ marginBottom: 4 }}>
                    Kill <strong>{shown}</strong> (pid {s.pid})? Its child process ends.
                  </div>
                  <button
                    onClick={() => void kill(s.name)}
                    style={{
                      fontSize: 11,
                      padding: "2px 8px",
                      marginRight: 4,
                      cursor: "pointer",
                      // The strong step, not `danger`: `danger` is picked to
                      // be READ as text on a panel, which leaves it too pale
                      // to sit UNDER white — 3.09:1 in dark, where the strong
                      // step is 10.02:1.
                      background: theme.dangerStrong,
                      color: theme.onAccent,
                      border: "none",
                      borderRadius: 3,
                    }}
                  >
                    Kill
                  </button>
                  <button
                    ref={cancelRef}
                    onClick={() => setPending(null)}
                    style={{
                      fontSize: 11,
                      padding: "2px 8px",
                      cursor: "pointer",
                      background: theme.surface,
                      color: theme.text,
                      border: `1px solid ${theme.border}`,
                      borderRadius: 3,
                    }}
                  >
                    Cancel
                  </button>
                </div>
              ) : (
                <button
                  onClick={() => onSelect(s.name)}
                  onContextMenu={(e) => {
                    e.preventDefault();
                    setPending({ name: s.name, pid: s.pid });
                  }}
                  style={{
                    flex: 1,
                    minWidth: 0,
                    textAlign: "left",
                    padding: "6px 8px",
                    border: "none",
                    borderRadius: 4,
                    background: s.name === active ? theme.accent : "transparent",
                    color: s.name === active ? theme.onAccent : "inherit",
                    cursor: "pointer",
                    fontSize: 13,
                  }}
                  title={`${s.view_cols}x${s.view_rows}, pid ${s.pid}, ${s.nclients} client(s) — right-click kills (asks first)`}
                >
                  {shown}
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
              )}
              {pending?.name !== s.name && onToggleMute && (
                <button
                  aria-label={
                    muted?.has(s.name) === true
                      ? `unmute notifications for ${shown}`
                      : `mute notifications for ${shown}`
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
          );
        })}
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
