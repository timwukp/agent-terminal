// Window shell: sidebar / terminal / claude panel (app/design/ux-spec.md).
// Claude panel fills in with PR6-PR9.
import { useCallback, useMemo, useState } from "react";
import TerminalView from "./terminal/Terminal";
import { TauriTransport } from "./terminal/transport";
import Sidebar from "./sidebar/Sidebar";
import { TauriControlApi } from "./sidebar/api";

export default function App() {
  const transport = useMemo(() => new TauriTransport(), []);
  const api = useMemo(() => new TauriControlApi(), []);
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

  const select = (name: string) => {
    setClosed(undefined);
    setStdinError(null);
    setActive(name);
    setFocusNonce((n) => n + 1);
  };
  // Stable identity: Terminal re-attaches when this changes.
  const onStdinError = useCallback((m: string) => setStdinError(m), []);

  return (
    <div style={{ display: "flex", height: "100vh", margin: 0, fontFamily: "system-ui" }}>
      <aside style={{ width: 220, borderRight: "1px solid #ccc", padding: 8 }}>
        <Sidebar api={api} active={active} onSelect={select} />
      </aside>
      <main style={{ flex: 1, position: "relative", background: "#1e2228" }}>
        {active === null ? (
          <p style={{ color: "#888", padding: 16, fontSize: 13 }}>
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
                  background: "#7f1d1d",
                  color: "#fff",
                  fontSize: 12,
                }}
              >
                input not delivered: {stdinError}
              </p>
            )}
          </>
        ) : (
          <p style={{ color: "#ccc", padding: 16, fontSize: 13 }}>
            {closed === null ? "session ended" : `connection closed: ${closed}`}
          </p>
        )}
      </main>
      <aside style={{ width: 260, borderLeft: "1px solid #ccc", padding: 8 }}>
        <h2 style={{ fontSize: 14, margin: "4px 0" }}>Claude</h2>
        <p style={{ fontSize: 12, color: "#888" }}>tokens / hooks / security land in PR6-PR9</p>
      </aside>
    </div>
  );
}
