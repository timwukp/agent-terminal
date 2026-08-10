// Window shell: sidebar / terminal / claude panel (app/design/ux-spec.md).
// Claude panel fills in with PR6-PR9.
import { useMemo, useState } from "react";
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

  const select = (name: string) => {
    setClosed(undefined);
    setActive(name);
  };

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
          <TerminalView
            key={active} // full remount on switch: drop + reconnect + ATTACH
            transport={transport}
            session={active}
            onClosed={setClosed}
          />
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
