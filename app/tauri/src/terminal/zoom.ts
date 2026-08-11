// Deriving "is a pane zoomed?" from MSG_LAYOUT.
//
// MSG_LAYOUT carries no zoom flag (proto.h:0x35 — view_cols, view_rows,
// active_id, npanes, then per-pane rects). Only SESSION_LIST2 reports
// `zoomed`, and that rides the *control* connection, not the attach one,
// so the terminal view cannot read it without a second source of truth.
//
// It does not need one, because the daemon's own zoom implementation makes
// the state visible in the geometry it already sends: while zoomed, the
// zoomed pane's applied rect is the full view (session.c:588 sets
// x=0, y=0, cols=view_cols, rows=view_rows) while every other pane keeps
// its tree rect. Unzoomed panes tile without overlapping, so "≥2 panes and
// one of them covers the whole view" happens only under zoom.
//
// Preferred over adding a protocol field: it is an observation about state
// the daemon already publishes, and a new field would need a C PR, a
// capability bit, and a fallback for older daemons for information already
// on the wire.

import type { PaneRect } from "./transport";

/** The id of the zoomed pane, or null when not zoomed. */
export function zoomedPaneId(panes: PaneRect[], viewCols: number, viewRows: number): number | null {
  // One pane always fills the view; that is not zoom, it is a single pane.
  if (panes.length < 2) return null;
  if (viewCols <= 0 || viewRows <= 0) return null;
  const full = panes.filter((p) => p.x === 0 && p.y === 0 && p.cols === viewCols && p.rows === viewRows);
  // Exactly one: two panes both claiming the whole view would mean the
  // rects are not what this function assumes, so report nothing rather
  // than pick arbitrarily.
  return full.length === 1 ? full[0].id : null;
}
