// History backfill: make the wheel scroll back past the attach point.
//
// The attach snapshot is only the visible screen; everything that ever
// scrolled off lives daemon-side (scrollback.c: a ring of the newest
// SB_MEM_LINES_DEFAULT lines over an append-only log holding the rest,
// the whole of it announced as sb_lines on the snapshot). xterm has no
// way to prepend into its
// buffer, so history must be written BEFORE the snapshot repaint — which
// forces an ordering dance at attach:
//
//   snapshot arrives → stash it, page history in (≤1000 lines/request,
//   the daemon's cap) → write pages oldest-first → then the stashed
//   repaint → then live output that queued up meanwhile.
//
// Live output cannot be written during the fetch or the history would
// land BELOW it; it queues here and flushes after the repaint, in order.
//
// Paging stops at the sb_lines the snapshot announced: lines pushed to
// the ring after that moment are exactly the ones the queued live output
// (or the repaint itself) already carries — fetching them would show
// everything from the fetch window twice.
//
// A timer guards the whole fetch: on any response gap the stash is
// painted without (further) history, because a terminal that renders
// late is an inconvenience but a terminal that renders never is an
// outage. Late pages after that are dropped — history written after the
// repaint would corrupt the screen.

/** Sinks the machine drives. All calls are synchronous; ordering is the
 * whole point, so nothing here may await. */
export interface BackfillSinks {
  /** One history line: ANSI text without a trailing newline. */
  writeLine(line: Uint8Array): void;
  /** Scroll every written history line off the visible screen. Called
   * between the last writeLine and applySnapshot, only when lines were
   * written: the repaint clears the visible screen, and history still
   * sitting on it (the last rows-1 lines) would be erased, not scrolled
   * — a silent gap right above the live screen, in the very lines the
   * user is most likely to scroll up looking for. */
  padToScrollback(): void;
  /** The (latest) stashed snapshot repaint, exactly once. */
  applySnapshot(cols: number, rows: number, blob: Uint8Array): void;
  /** Live bytes, after applySnapshot. */
  writeOutput(bytes: Uint8Array): void;
  /** Ask the daemon for one page (transport.scrollbackReq). */
  request(startSeq: number, maxLines: number): void;
}

/** The daemon serves at most this many lines per MSG_SCROLLBACK_DATA
 * (server.c clamps maxn to 1000). */
export const PAGE_LINES = 1000;
/** Fetch at most this much history.
 *
 * It used to be 10,000 because that was the daemon's in-memory ring
 * (SB_MEM_LINES_DEFAULT) and asking deeper could only come back empty.
 * The daemon now serves any depth from the log (sb_fetch_deep), so this
 * number stopped describing the daemon and became purely a client-side
 * budget — which makes the number itself the whole decision:
 *
 *   xterm stores a line as a Uint32Array of 3 words per cell allocated to
 *   the line's width, i.e. 12 B/cell. At 124 cols that is 1,488 B/line;
 *   at 155 cols, 1,860 B. So 25,000 lines is 37.2 MB per tab at 124 cols
 *   and 46.5 MB at 155 — and 100,000 would be 148.8 / 186.0 MB, per tab.
 *   The socket cost is small either way (177.9 B/line measured across 28
 *   real logs, so ~4.4 MB and 25 round trips for a 25,000-line attach).
 *
 * Memory is the binding constraint and it is paid EAGERLY: xterm cannot
 * prepend to its buffer (see the header), so history has to be written
 * before the repaint, which means at attach and not when the user
 * actually scrolls. Deeper-on-demand needs a different mechanism than
 * this module, and until it exists this constant is the depth a user can
 * reach: 2.5x the old one, and 27% of the largest real session measured
 * on this machine (93,374 lines / 18.1 MB). */
export const FETCH_MAX = 25000;
/** A fetch stalled this long paints without history. */
export const STALL_MS = 2000;

interface Stash {
  cols: number;
  rows: number;
  blob: Uint8Array;
}

export class Backfill {
  private sinks: BackfillSinks;
  /** "waiting" until the first snapshot; "fetching" while paging; "live"
   * once the repaint is on screen and passthrough begins. */
  private state: "waiting" | "fetching" | "live" = "waiting";
  private disposed = false;
  private stash: Stash | null = null;
  private queued: Uint8Array[] = [];
  /** First seq not yet requested / next page start. */
  private nextStart = 0;
  private wroteAny = false;
  /** Seq of the oldest line actually written; see oldestWritten(). */
  private oldest: number | null = null;
  /** One past the last seq to show (sb_lines at snapshot time). */
  private target = 0;
  private timer: ReturnType<typeof setTimeout> | null = null;

  constructor(sinks: BackfillSinks) {
    this.sinks = sinks;
  }

  onOutput(bytes: Uint8Array): void {
    if (this.disposed) return;
    if (this.state === "live") this.sinks.writeOutput(bytes);
    else this.queued.push(bytes);
  }

  onSnapshot(cols: number, rows: number, sbLines: number, blob: Uint8Array): void {
    if (this.disposed) return;
    if (this.state === "live") {
      // Geometry change mid-session: a plain repaint, no history — what
      // scrolled off since attach is already in xterm's own buffer.
      this.sinks.applySnapshot(cols, rows, blob);
      return;
    }
    this.stash = { cols, rows, blob };
    if (this.state === "fetching") return; // newer screen, same fetch
    if (sbLines <= 0) {
      this.finish();
      return;
    }
    this.state = "fetching";
    this.target = sbLines;
    this.nextStart = Math.max(0, sbLines - FETCH_MAX);
    this.requestNext();
  }

  onScrollback(firstSeq: number, lines: Uint8Array[]): void {
    if (this.disposed || this.state !== "fetching") return; // late page after fallback
    this.clearTimer();
    // The ring may have evicted past our start (firstSeq > nextStart) and
    // may have grown past the snapshot (lines beyond target) — show only
    // [firstSeq, target).
    const show = Math.min(lines.length, Math.max(0, this.target - firstSeq));
    for (let i = 0; i < show; i++) this.sinks.writeLine(lines[i]);
    if (show > 0) {
      this.wroteAny = true;
      if (this.oldest === null) this.oldest = firstSeq;
    }
    const covered = firstSeq + lines.length;
    if (lines.length === 0 || covered >= this.target) {
      this.finish();
      return;
    }
    this.nextStart = covered;
    this.requestNext();
  }

  /** The stall timer fired (or the host gave up): paint what we have. */
  onStall(): void {
    if (!this.disposed && this.state !== "live") this.finish();
  }

  /** The attach-time fetch is over (or was never needed): live output goes
   * straight to the view and no page is outstanding.
   *
   * The deep-history offer is gated on this. Both machines consume
   * MSG_SCROLLBACK_DATA, which carries no requester id, so exactly one may
   * be waiting at a time — and the pill is reachable mid-fetch, because the
   * first page already gives it a number to show and the user can scroll to
   * the top while the remaining 24 are still in flight. Opening a viewer
   * there would let it eat a page this machine asked for: the live history
   * ends up truncated and the window anchors on a seq nobody chose. */
  isLive(): boolean {
    return this.state === "live";
  }

  /** Seq of the oldest history line this machine actually wrote into the
   * view, or null when it wrote none (a session with no history, or a
   * fetch that stalled before its first page landed).
   *
   * The deep-history viewer (historyView.ts) needs this to know where the
   * live buffer begins, and it has to be the OBSERVED seq rather than the
   * requested floor (`sb_lines - FETCH_MAX`): the daemon answers from the
   * oldest seq the log still holds, which rotation can move forward after
   * the request. A viewer that abutted the *request* would then leave a
   * hole between its last line and the live buffer's first while
   * presenting the two as continuous — the one failure this feature must
   * not have, since being trusted about what was said is its whole job. */
  oldestWritten(): number | null {
    return this.oldest;
  }

  /** Component unmount: nothing may fire afterwards. */
  dispose(): void {
    this.disposed = true;
    this.clearTimer();
    this.queued.length = 0;
    this.stash = null;
  }

  private requestNext(): void {
    this.sinks.request(this.nextStart, Math.min(PAGE_LINES, this.target - this.nextStart));
    this.clearTimer();
    this.timer = setTimeout(() => this.onStall(), STALL_MS);
  }

  private finish(): void {
    this.clearTimer();
    this.state = "live";
    if (this.wroteAny) this.sinks.padToScrollback();
    if (this.stash) {
      this.sinks.applySnapshot(this.stash.cols, this.stash.rows, this.stash.blob);
      this.stash = null;
    }
    for (const bytes of this.queued) this.sinks.writeOutput(bytes);
    this.queued.length = 0;
  }

  private clearTimer(): void {
    if (this.timer !== null) {
      clearTimeout(this.timer);
      this.timer = null;
    }
  }
}
