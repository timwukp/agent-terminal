// Deep history: a window over the whole log, for the depth the live
// terminal structurally cannot hold.
//
// backfill.ts writes FETCH_MAX lines into xterm at attach and stops there,
// and the comment on that constant says why the number cannot simply grow:
// xterm has no way to prepend into its buffer, so history has to be written
// BEFORE the snapshot repaint — i.e. eagerly, at attach — and a line costs
// 12 B/cell x cols whether the user ever scrolls to it or not. On the
// largest session measured on this machine (93,374 lines) the live terminal
// therefore reaches 27% of the history and the rest was only ever available
// through `agent-terminal history`.
//
// This module is the different mechanism that gap needs. It does not touch
// the live terminal: it drives a SECOND, read-only terminal showing a
// bounded WINDOW_LINES window that can sit anywhere in [0, total), moved by
// re-anchoring — reset the view, refetch from the new start. Appending is
// free in xterm and prepending is impossible, so a symmetric "reset and
// refetch" is the only move that works in both directions, and one
// mechanism with one set of edge cases beats two.
//
// The daemon needs no new message: MSG_SCROLLBACK_REQ has served any depth
// from the log since sb_fetch_deep (a seek to the nearest sparse-index
// record plus a bounded sweep, ~285 KB and 12.1 ms measured on the 18.1 MB
// log), and the request rides the connection the session is already
// attached on. Pages arrive on the same channel as live output, so the host
// has to route them, and each machine also drops a page it did not ask for
// (Backfill once it is live; this one whenever `loading` is false).
//
// That is not by itself enough, and the comment here used to claim it was:
// two machines can both be waiting, and then dropping is no defence because
// each page IS wanted — by the other one. What makes the route decidable is
// that the host never lets a viewer exist while the backfill is still
// fetching (Terminal.tsx refreshDeeper, gated on Backfill.isLive()). This
// machine's `wants()` then only ever disambiguates a page from a live
// backfill, which is the case where dropping is correct.
//
// What this deliberately does NOT do is follow live output. The window is
// history, `total` is what the last snapshot announced, and the live
// terminal is still running behind the overlay — a viewer that also tailed
// would need to reconcile the two, and the question being answered here is
// "what did we say an hour ago", not "what is happening now".

/** Lines held in the viewer at once. The same 12 B/cell arithmetic as
 * FETCH_MAX applies (backfill.ts), so at 124 cols this window is 5.95 MB
 * and at 155 cols 7.44 MB — paid only while the viewer is open, and freed
 * with its terminal when it closes. Bounded rather than growing on purpose:
 * a user who pages back through a 90,000-line log must not accumulate
 * 90,000 lines of client-side buffer to do it. */
export const WINDOW_LINES = 4000;
/** Re-anchor stride. Half a window, so a move keeps half of what was on
 * screen: the line you were reading is still there, which is the difference
 * between paging and losing your place. */
export const STEP_LINES = WINDOW_LINES / 2;
/** The daemon serves at most this many lines per MSG_SCROLLBACK_DATA
 * (server.c clamps maxn to 1000) — same cap backfill pages against. */
export const PAGE_LINES = 1000;
/** A page not answered within this long stops the load and says so. The
 * live terminal is unaffected, so unlike backfill's stall there is nothing
 * to rescue by guessing — the honest move is to stop and offer a retry. */
export const STALL_MS = 3000;

/** Where a move wants the viewport, as an absolute sequence number: `top`
 * puts that line at the top of the viewport (continue reading downward),
 * `bottom` puts it at the bottom (continue reading upward). Absolute
 * because the daemon may answer from a later seq than we asked for, which
 * moves the anchor after the intent was formed. */
interface ScrollIntent {
  seq: number;
  where: "top" | "bottom";
}

export interface HistoryState {
  /** First seq in the window. */
  anchor: number;
  /** Lines currently written into the view. */
  loaded: number;
  /** Lines of history the session has, as announced by the last snapshot. */
  total: number;
  /** A page is outstanding. */
  loading: boolean;
  /** A page never came back; `retry` is the way out. */
  stalled: boolean;
  /** Older / newer history exists outside this window. */
  canEarlier: boolean;
  canLater: boolean;
}

export interface HistoryViewSinks {
  /** Empty the view: a re-anchor rewrites it from scratch. */
  reset(): void;
  /** One history line: ANSI text without a trailing newline. */
  writeLine(line: Uint8Array): void;
  /** Ask the daemon for one page (transport.scrollbackReq). */
  request(startSeq: number, maxLines: number): void;
  /** Put `bufferLine` (0-based, within the loaded window) at the top or
   * bottom of the viewport. The caller knows its own row count; the
   * machine deliberately does not. */
  scrollTo(bufferLine: number, where: "top" | "bottom"): void;
  /** Render state changed. Called on every transition, including the ones
   * that only flip `loading` — the UI disables its controls on that. */
  onState(state: HistoryState): void;
}

export class HistoryView {
  private sinks: HistoryViewSinks;
  private total: number;
  private anchor = 0;
  private loaded = 0;
  private loading = false;
  private stalled = false;
  private disposed = false;
  private intent: ScrollIntent | null = null;
  private timer: ReturnType<typeof setTimeout> | null = null;

  /** `total` is the sb_lines the snapshot announced. */
  constructor(sinks: HistoryViewSinks, total: number) {
    this.sinks = sinks;
    this.total = Math.max(0, total);
  }

  /** A later snapshot announced more history (the session kept running, or
   * geometry changed and the daemon repainted). Only the ceiling moves;
   * the window stays where the user put it. */
  setTotal(total: number): void {
    if (this.disposed || total <= this.total) return;
    this.total = total;
    this.emit();
  }

  /** Open at `fromSeq`, showing the END of that window — the caller opens
   * this because it ran out of history going UP, so the line it wants
   * adjacent to what it was reading is the last one here. */
  open(fromSeq: number): void {
    this.seek(fromSeq, { seq: fromSeq + WINDOW_LINES - 1, where: "bottom" });
  }

  /** One window older. The line that was at the top of the old window ends
   * up at the bottom of the new one, so the text is continuous across the
   * move. */
  earlier(): void {
    if (this.loading || this.anchor <= 0) return;
    const keep = this.anchor;
    this.seek(Math.max(0, this.anchor - STEP_LINES), { seq: keep, where: "bottom" });
  }

  /** One window newer, with the same overlap in the other direction: the
   * old window's last line lands at the top. */
  later(): void {
    if (this.loading || this.anchor + this.loaded >= this.total) return;
    const keep = this.anchor + this.loaded;
    this.seek(this.anchor + STEP_LINES, { seq: keep, where: "top" });
  }

  /** The very beginning of the log. */
  toOldest(): void {
    if (this.loading) return;
    this.seek(0, { seq: 0, where: "top" });
  }

  /** The end of the history the last snapshot announced — the join with
   * what the live terminal is showing. */
  toNewest(): void {
    if (this.loading) return;
    const start = Math.max(0, this.total - WINDOW_LINES);
    this.seek(start, { seq: this.total - 1, where: "bottom" });
  }

  /** A page stalled and the user asked again: continue the same window
   * from where it stopped, rather than restarting it (the lines already on
   * screen are correct — losing them would be a worse answer than a
   * partial window). */
  retry(): void {
    if (this.disposed || this.loading) return;
    this.stalled = false;
    // Same shape as seek(): a successful ask has to be announced. Without the
    // emit the two flags this method changes stay invisible — the red
    // "stalled — retry" button remains on screen for the whole 3 s a page is
    // in flight, and a second click on it does nothing at all (the `loading`
    // guard above swallows it), so the one control offered here reads as
    // broken exactly when it is working.
    if (this.requestNext()) this.emit();
    else this.finish();
  }

  /** True while a page this machine asked for is outstanding. The host
   * routes MSG_SCROLLBACK_DATA on this, so it is the whole reason a live
   * Backfill and an open viewer cannot consume each other's pages. */
  wants(): boolean {
    return this.loading;
  }

  onPage(firstSeq: number, lines: Uint8Array[]): void {
    if (this.disposed || !this.loading) return; // a page we did not ask for
    this.clearTimer();
    if (lines.length === 0) {
      // The log genuinely ends here: rotation dropped what we asked for, or
      // `total` (a snapshot's number) is ahead of what the log still holds.
      this.finish();
      return;
    }
    if (this.loaded === 0) {
      // The daemon answers from the oldest seq it still has, which can be
      // later than the ask after a rotation. Follow it, exactly as
      // backfill does, so the position labels describe the lines actually
      // on screen.
      this.anchor = firstSeq;
    } else if (firstSeq !== this.anchor + this.loaded) {
      // A gap would put non-adjacent lines next to each other and make
      // every position after it a lie. Stop with what is verifiably
      // contiguous.
      this.finish();
      return;
    }
    const room = WINDOW_LINES - this.loaded;
    const n = Math.min(lines.length, room);
    for (let i = 0; i < n; i++) this.sinks.writeLine(lines[i]);
    this.loaded += n;
    if (this.loaded >= WINDOW_LINES || !this.requestNext()) this.finish();
    // Mid-window: the position readout has to follow the lines that landed,
    // or a 4-page window reports its first page's extent until the last one
    // arrives — and after a rotation it would report the wrong anchor
    // entirely (the ask, not the answer).
    else this.emit();
  }

  /** The stall timer fired. */
  onStall(): void {
    if (this.disposed || !this.loading) return;
    this.loading = false;
    this.stalled = true;
    this.clearTimer();
    this.applyIntent();
    this.emit();
  }

  dispose(): void {
    this.disposed = true;
    this.clearTimer();
    this.intent = null;
  }

  private seek(startSeq: number, intent: ScrollIntent): void {
    if (this.disposed || this.total <= 0) return;
    // Never anchor past the end: the last window is [total-WINDOW, total).
    const maxAnchor = Math.max(0, this.total - WINDOW_LINES);
    this.anchor = Math.min(Math.max(0, Math.floor(startSeq)), maxAnchor);
    this.loaded = 0;
    this.stalled = false;
    this.intent = intent;
    this.sinks.reset();
    if (!this.requestNext()) {
      this.finish();
      return;
    }
    this.emit();
  }

  /** Ask for the next page of the current window. False when the window is
   * already as full as the log and the cap allow — i.e. nothing to ask. */
  private requestNext(): boolean {
    const end = Math.min(this.anchor + WINDOW_LINES, this.total);
    const want = Math.min(PAGE_LINES, end - (this.anchor + this.loaded));
    if (want <= 0) return false;
    this.loading = true;
    this.sinks.request(this.anchor + this.loaded, want);
    this.clearTimer();
    this.timer = setTimeout(() => this.onStall(), STALL_MS);
    return true;
  }

  private finish(): void {
    this.clearTimer();
    this.loading = false;
    this.applyIntent();
    this.emit();
  }

  private applyIntent(): void {
    const intent = this.intent;
    this.intent = null;
    if (intent === null || this.loaded === 0) return;
    const line = Math.min(Math.max(0, intent.seq - this.anchor), this.loaded - 1);
    this.sinks.scrollTo(line, intent.where);
  }

  private emit(): void {
    this.sinks.onState({
      anchor: this.anchor,
      loaded: this.loaded,
      total: this.total,
      loading: this.loading,
      stalled: this.stalled,
      canEarlier: this.anchor > 0,
      canLater: this.anchor + this.loaded < this.total,
    });
  }

  private clearTimer(): void {
    if (this.timer !== null) {
      clearTimeout(this.timer);
      this.timer = null;
    }
  }
}
