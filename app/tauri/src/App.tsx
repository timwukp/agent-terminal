// Window shell: sidebar / terminal / claude panel (app/design/ux-spec.md).
// Claude panel fills in with PR6-PR9.
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import TerminalView from "./terminal/Terminal";
import { TauriTransport } from "./terminal/transport";
import { windowTitle } from "./terminal/viewControls";
import Sidebar from "./sidebar/Sidebar";
import { TauriControlApi } from "./sidebar/api";
import TokenPanel from "./panels/TokenPanel";
import { TauriUsageApi } from "./panels/usageApi";
import HooksPanel from "./panels/HooksPanel";
import { TauriHooksApi } from "./panels/hooksApi";
import { decideNotify, deliverNotification } from "./notify";
import { makeOneShotSet } from "./terminal/viewControls";
import { theme } from "./theme";
import ThemeSwitch from "./ThemeSwitch";

export default function App() {
  const transport = useMemo(() => new TauriTransport(), []);
  const api = useMemo(() => new TauriControlApi(), []);
  const usageApi = useMemo(() => new TauriUsageApi(), []);
  const hooksApi = useMemo(() => new TauriHooksApi(), []);
  // The inactive tab's panel unmounts, which stops its polling for free.
  const [claudeTab, setClaudeTab] = useState<"usage" | "hooks">("usage");

  // Sessions THIS GUI created get fitted to the window once, at first
  // attach: their 80×24 default was our choice, and we are their only
  // viewer at birth. One-shot — a later re-select must not re-impose
  // geometry the user may have changed. CLI-created sessions are never
  // in this set, so they are never auto-resized.
  const autoFitNewborn = useMemo(() => makeOneShotSet(), []);
  const [active, setActive] = useState<string | null>(
    () => new URLSearchParams(window.location.search).get("session"),
  );
  const [closed, setClosed] = useState<string | null | undefined>(undefined);
  // Undelivered input must be visible where the typing is happening. A
  // console message is not: a build pairing fixed Rust with a stale
  // dist/ rejected every keystroke and looked like a broken product.
  const [stdinError, setStdinError] = useState<string | null>(null);

  // Every sidebar interaction leaves DOM focus on the button that was
  // clicked, so the terminal has to be told to take it back. Bumped even
  // when the name is unchanged: clicking the active session does not
  // remount TerminalView, and that case is exactly "I clicked the
  // session, then typed, and nothing happened".
  const [focusNonce, setFocusNonce] = useState(0);

  // ⌘-Tab, the Dock, and screenshots all read the window title; make it
  // say which session this window is looking at.
  useEffect(() => {
    document.title = windowTitle(active);
  }, [active]);

  // Open by default now that it has a tenant (the token panel); the
  // collapse handle remains for people who want the pixels back.
  const [claudeOpen, setClaudeOpen] = useState(true);

  // Notifications (app/design/notifications.md): per-session mute, and a
  // sidebar badge for turns that completed while the window was unfocused
  // — the badge is also the whole story when OS delivery is unavailable
  // (unbundled macOS dev binaries cannot post to the notification center).
  const [muted, setMuted] = useState<ReadonlySet<string>>(new Set());
  const [done, setDone] = useState<ReadonlySet<string>>(new Set());
  // Latest-refs: onTurnDone is read through a ref in TerminalView, so it
  // is deliberately not identity-stable — but reading state directly here
  // would still close over the render it was created in.
  const mutedRef = useRef(muted);
  mutedRef.current = muted;
  const activeRef = useRef(active);
  activeRef.current = active;

  const onTurnDone = (_reason: "bell" | "idle", lastLine: string) => {
    const name = activeRef.current;
    if (name === null) return;
    const d = decideNotify(document.hasFocus(), mutedRef.current.has(name));
    if (d.badge) setDone((s) => new Set(s).add(name));
    if (d.notify) void deliverNotification(name, lastLine);
  };

  const toggleMute = (name: string) =>
    setMuted((s) => {
      const next = new Set(s);
      if (next.has(name)) next.delete(name);
      else next.add(name);
      return next;
    });

  const select = (name: string) => {
    setClosed(undefined);
    setStdinError(null);
    setActive(name);
    setFocusNonce((n) => n + 1);
    // Selecting a session is looking at it; its "finished" mark is served.
    setDone((s) => {
      if (!s.has(name)) return s;
      const next = new Set(s);
      next.delete(name);
      return next;
    });
  };
  // Stable identity: Terminal re-attaches when this changes.
  const onStdinError = useCallback((m: string) => setStdinError(m), []);

  return (
    <div
      style={{
        display: "flex",
        height: "100vh",
        margin: 0,
        fontFamily: "system-ui",
        // One surface system end to end (theme.ts), whichever theme is
        // on: light chrome around a dark terminal read as two different
        // apps, and so would the reverse.
        background: theme.bg,
        color: theme.text,
      }}
    >
      <aside
        style={{
          width: 220,
          borderRight: `1px solid ${theme.border}`,
          padding: 8,
          display: "flex",
          flexDirection: "column",
          minHeight: 0,
        }}
      >
        <div style={{ flex: 1, minHeight: 0 }}>
          <Sidebar
            api={api}
            active={active}
            onSelect={select}
            onCreated={(name) => autoFitNewborn.add(name)}
            muted={muted}
            done={done}
            onToggleMute={toggleMute}
          />
        </div>
        {/* App-level, so it sits outside the session list rather than
            under the template buttons that create sessions. */}
        <ThemeSwitch />
      </aside>
      <main style={{ flex: 1, position: "relative", background: theme.bgMain }}>
        {active === null ? (
          <p style={{ color: theme.textMuted, padding: 16, fontSize: 13 }}>
            select a session, or create one from a template
          </p>
        ) : closed === undefined ? (
          <>
            <TerminalView
              key={active} // full remount on switch: drop + reconnect + ATTACH
              transport={transport}
              session={active}
              onClosed={setClosed}
              onStdinError={onStdinError}
              focusNonce={focusNonce}
              onTurnDone={onTurnDone}
              autoFit={() => autoFitNewborn.consume(active)}
            />
            {stdinError !== null && (
              <p
                role="alert"
                style={{
                  position: "absolute",
                  bottom: 0,
                  left: 0,
                  right: 0,
                  margin: 0,
                  padding: "6px 10px",
                  background: theme.dangerStrong,
                  color: theme.onAccent,
                  fontSize: 12,
                }}
              >
                input not delivered: {stdinError}
              </p>
            )}
          </>
        ) : (
          <p style={{ color: theme.textMuted, padding: 16, fontSize: 13 }}>
            {closed === null ? "session ended" : `connection closed: ${closed}`}
          </p>
        )}
      </main>
      <aside
        style={{
          width: claudeOpen ? 260 : 24,
          borderLeft: `1px solid ${theme.border}`,
          padding: claudeOpen ? 8 : 0,
          overflow: claudeOpen ? "auto" : "hidden",
        }}
      >
        <button
          onClick={() => setClaudeOpen((v) => !v)}
          aria-expanded={claudeOpen}
          title={claudeOpen ? "Collapse the Claude panel" : "Expand the Claude panel"}
          style={{
            width: claudeOpen ? "auto" : "100%",
            height: claudeOpen ? "auto" : "100%",
            border: "none",
            background: "transparent",
            cursor: "pointer",
            fontSize: 12,
            color: theme.textMuted,
            padding: claudeOpen ? "2px 4px" : 0,
          }}
        >
          {claudeOpen ? "»" : "«"}
        </button>
        {claudeOpen && (
          <>
            <div role="tablist" style={{ display: "flex", gap: 4, margin: "2px 0 6px" }}>
              {(["usage", "hooks"] as const).map((tab) => (
                <button
                  key={tab}
                  role="tab"
                  aria-selected={claudeTab === tab}
                  onClick={() => setClaudeTab(tab)}
                  style={{
                    flex: 1,
                    padding: "3px 0",
                    fontSize: 11,
                    cursor: "pointer",
                    border: `1px solid ${theme.border}`,
                    borderRadius: 4,
                    background: claudeTab === tab ? theme.accent : "transparent",
                    color: claudeTab === tab ? theme.onAccent : theme.textMuted,
                  }}
                >
                  {tab === "usage" ? "Usage" : "Hooks"}
                </button>
              ))}
            </div>
            {claudeTab === "usage" ? <TokenPanel api={usageApi} /> : <HooksPanel api={hooksApi} />}
          </>
        )}
      </aside>
    </div>
  );
}
