// Window shell: sidebar / terminal / claude panel (app/design/ux-spec.md).
// PR1 renders the empty regions; each fills in over PR3-PR8.
export default function App() {
  return (
    <div style={{ display: "flex", height: "100vh", margin: 0, fontFamily: "system-ui" }}>
      <aside style={{ width: 220, borderRight: "1px solid #ccc", padding: 8 }}>
        <h2 style={{ fontSize: 14, margin: "4px 0" }}>Sessions</h2>
        <p style={{ fontSize: 12, color: "#888" }}>connects in PR4</p>
      </aside>
      <main style={{ flex: 1, padding: 8 }}>
        <p style={{ fontSize: 12, color: "#888" }}>terminal attaches in PR3</p>
      </main>
      <aside style={{ width: 260, borderLeft: "1px solid #ccc", padding: 8 }}>
        <h2 style={{ fontSize: 14, margin: "4px 0" }}>Claude</h2>
        <p style={{ fontSize: 12, color: "#888" }}>tokens / hooks / security land in PR6-PR9</p>
      </aside>
    </div>
  );
}
