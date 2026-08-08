/* composite.h — render a multi-pane session into one ANSI frame.
 *
 * A composited frame is a byte blob of the same kind as a snapshot, so it
 * rides the existing MSG_OUTPUT path and needs no client rendering code: an
 * old client attached to a 3-pane session draws the composite perfectly (it
 * just cannot create splits). Talks to panes only through the public vt.h. */
#ifndef AT_COMPOSITE_H
#define AT_COMPOSITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "session.h"

/* True when the session renders via the compositor (>= 2 panes). At exactly
 * one pane the raw PTY tee is used unchanged — perfect fidelity for free,
 * and pinned by the byte-identical guard test. */
bool session_should_composite(const session *s);

/* Append a frame repainting the given panes' dirty rows (all rows if
 * `full`), plus dividers and the active pane's cursor, to *out (malloc'd,
 * caller frees). Returns the frame length, 0 if nothing to draw or OOM.
 * Does NOT clear pane damage — the caller clears once per frame, after
 * every attached client got the bytes. */
size_t composite_frame(session *s, bool full, char **out);

#endif
