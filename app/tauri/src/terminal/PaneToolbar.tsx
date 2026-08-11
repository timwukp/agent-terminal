// Pane actions. Each button maps 1:1 to an existing protocol message, so
// this file adds no wire surface (ux-spec.md).
//
// Split and close act on the ACTIVE pane (PANE_ACTIVE), not on a pane the
// user picked here — the daemon owns which pane is active, and a toolbar
// that guessed differently would act on the wrong one.

export interface PaneToolbarProps {
  onSplitVertical(): void;
  onSplitHorizontal(): void;
  onZoomToggle(): void;
  onClose(): void;
  /** Panes in the session; close is hidden at 1 because closing the only
   * pane means killing the session, which belongs to the sidebar's
   * confirm-guarded path, not to a single-click toolbar button. */
  paneCount: number;
  zoomed: boolean;
}

const btn: React.CSSProperties = {
  background: "#2d3239",
  color: "#dfe4ea",
  border: "1px solid #3f464f",
  borderRadius: 4,
  cursor: "pointer",
  fontSize: 12,
  lineHeight: "18px",
  padding: "1px 7px",
};

export default function PaneToolbar({
  onSplitVertical,
  onSplitHorizontal,
  onZoomToggle,
  onClose,
  paneCount,
  zoomed,
}: PaneToolbarProps) {
  return (
    <div
      style={{
        position: "absolute",
        top: 6,
        right: 6,
        display: "flex",
        gap: 4,
        // Above the overlay, and the only interactive layer in the region.
        zIndex: 2,
        opacity: 0.85,
      }}
    >
      <button
        style={btn}
        onClick={onSplitVertical}
        title="Split left/right — the split lives in the session (CLI viewers see it too)"
      >
        ▯▯
      </button>
      <button
        style={btn}
        onClick={onSplitHorizontal}
        title="Split top/bottom — the split lives in the session (CLI viewers see it too)"
      >
        ▤
      </button>
      <button
        style={{ ...btn, background: zoomed ? "#2b6cb0" : btn.background }}
        onClick={onZoomToggle}
        title={zoomed ? "Unzoom pane" : "Zoom active pane"}
        aria-pressed={zoomed}
      >
        {zoomed ? "🔍−" : "🔍+"}
      </button>
      {paneCount > 1 && (
        <button style={btn} onClick={onClose} title="Close active pane">
          ✕
        </button>
      )}
    </div>
  );
}
