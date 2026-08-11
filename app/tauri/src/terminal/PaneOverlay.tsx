// Active-pane highlight, drawn as one absolutely-positioned div per pane.
//
// Overlay divs rather than terminal cells, per ux-spec.md: the grid belongs
// to the daemon and every cell in it is real session content. Writing a
// border into cells would corrupt the very output being viewed.
//
// pointerEvents is none throughout — the click handler lives on the host so
// that clicks on the letter-box margin still focus the terminal, and an
// overlay that swallowed clicks would silently break click-to-focus.

import type { PaneRect } from "./transport";
import type { CellMetrics } from "./hittest";
import { paneRectToPixels } from "./overlay";

export interface PaneOverlayProps {
  panes: PaneRect[];
  activeId: number;
  metrics: CellMetrics | null;
  /** The scale xterm's element carries; overlays must match it exactly or
   * they drift from the text they outline. */
  scale: number;
}

export default function PaneOverlay({ panes, activeId, metrics, scale }: PaneOverlayProps) {
  // A single pane has no ambiguity about where input goes, so an outline
  // would be decoration. No metrics means the private xterm shape moved:
  // degrade to no overlay rather than drawing boxes in wrong places.
  if (!metrics || panes.length < 2) return null;

  return (
    <div
      data-testid="pane-overlay"
      style={{
        position: "absolute",
        inset: 0,
        pointerEvents: "none",
        transform: `scale(${scale})`,
        transformOrigin: "top left",
      }}
    >
      {panes.map((p) => {
        const r = paneRectToPixels(p, metrics);
        const active = p.id === activeId;
        return (
          <div
            key={p.id}
            data-pane-id={p.id}
            data-active={active}
            style={{
              position: "absolute",
              left: r.left,
              top: r.top,
              width: r.width,
              height: r.height,
              boxSizing: "border-box",
              // Inset so the border sits inside the pane's own cells and
              // cannot cover a neighbour's leftmost column.
              border: active ? "1px solid #4a9eff" : "1px solid transparent",
              borderRadius: 2,
              transition: "border-color 120ms",
            }}
          />
        );
      })}
    </div>
  );
}
