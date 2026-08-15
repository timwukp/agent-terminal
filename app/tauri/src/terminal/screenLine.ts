// The one line of session output that leaves the terminal region: the text
// an OS notification uses to say WHAT finished. Split out of Terminal.tsx so
// it can be driven by a real xterm parser in a test — the notification body
// is arbitrary output from someone else's program, and what that program can
// put in it is a measurement, not an assumption (see screenLine.test.ts).

import type { Terminal as XTerm } from "@xterm/xterm";

/** Bottom-most non-empty row of the buffer. */
export function lastNonEmptyLine(term: XTerm): string {
  const buf = term.buffer.active;
  for (let y = buf.length - 1; y >= 0; y--) {
    const text = buf.getLine(y)?.translateToString(true).trim();
    if (text) return text;
  }
  return "";
}
