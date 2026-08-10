// Window shell: sidebar / terminal / claude panel (app/design/ux-spec.md).
// PR3 wires the terminal region; sidebar fills in with PR4, claude panel
// with PR6-PR9.
import { useMemo, useState } from "react";
import TerminalView from "./terminal/Terminal";
import { TauriTransport } from "./terminal/transport";

export default function App() {
  const transport = useMemo(() => new TauriTransport(), []);
  // PR4 replaces this with the LIST_SESSIONS2 sidebar; until then the
  // session name comes from the URL (?session=work) for manual testing.
  const [session] = useState(
    () => new URLSearchParams(window.location.search).get("session") ?? "main",
  );
  const [closed, setClosed] = useState<string | null | undefined>(undefined);

  return (
    <div style={{ display: "flex", height: "100vh", margin: 0, fontFamily: "system-ui" }}>
      <aside style={{ width: 220, borderRight: "1px solid #ccc", padding: 8 }}>
        <h2 style={{ fontSize: 14, margin: "4px 0" }}>Sessions</h2>
        <p style={{ fontSize: 12, color: "#888" }}>connects in PR4</p>
      </aside>
      <main style={{ flex: 1, position: "relative", background: "#1e2228" }}>
        {closed === undefined ? (
          <TerminalView transport={transport} session={session} onClosed={setClosed} />
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
