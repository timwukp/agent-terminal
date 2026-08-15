// Pane actions. Each button maps 1:1 to an existing protocol message, so
// this file adds no wire surface (ux-spec.md).
//
// Split and close act on the ACTIVE pane (PANE_ACTIVE), not on a pane the
// user picked here — the daemon owns which pane is active, and a toolbar
// that guessed differently would act on the wrong one.

import { theme } from "../theme";

export interface PaneToolbarProps {
  onSplitVertical(): void;
  onSplitHorizontal(): void;
  onZoomToggle(): void;
  onClose(): void;
  /** Resize the SESSION to this window — the only place the GUI ever
   * imposes geometry, because the user pressed it. Every attached
   * viewer reflows (say so in the tooltip, not in a surprise). */
  onFitToWindow(): void;
  /** Panes in the session; close is hidden at 1 because closing the only
   * pane means killing the session, which belongs to the sidebar's
   * confirm-guarded path, not to a single-click toolbar button. */
  paneCount: number;
  zoomed: boolean;
}

const btn: React.CSSProperties = {
  background: theme.raised,
  color: theme.text,
  border: `1px solid ${theme.raisedBorder}`,
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
  onFitToWindow,
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
        onClick={onFitToWindow}
        title="Fit session to window — resizes the session itself, every attached viewer reflows"
      >
        ⤢
      </button>
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
        style={{ ...btn, background: zoomed ? theme.accent : btn.background }}
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
