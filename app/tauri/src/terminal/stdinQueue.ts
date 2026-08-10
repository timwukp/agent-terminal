// Keystrokes must never be silently lost, and must never reorder.
//
// Two hazards this exists to close, both observed as "I typed and the
// character did not appear, so I typed it again":
//
//  1. Keys pressed before ATTACH completes. xterm accepts input the
//     moment it is rendered, but the Rust side has no attachment until
//     the handshake finishes; sending then fails. Such keys are held
//     here and flushed in order once open() is called.
//  2. A rejected send being discarded. `void transport.stdin(...)`
//     throws away both the error and the byte. Every failure goes to
//     onError instead, so a drop is visible rather than mysterious.
//
// Ordering: every send is chained onto one promise tail, so a byte can
// never overtake an earlier one even while a flush is in flight.

export interface StdinQueue {
  /** Queue (before open) or send (after) one chunk of input bytes. */
  push(bytes: Uint8Array): void;
  /** Attachment is live: flush what was held, in arrival order. */
  open(): void;
}

export function createStdinQueue(
  send: (bytes: Uint8Array) => Promise<void>,
  onError: (e: unknown) => void,
): StdinQueue {
  const held: Uint8Array[] = [];
  let opened = false;
  // Serializes sends; a failure is reported without breaking the chain,
  // so one dropped byte does not stall every later keystroke.
  let tail: Promise<void> = Promise.resolve();

  const enqueue = (bytes: Uint8Array) => {
    tail = tail.then(() => send(bytes)).catch(onError);
  };

  return {
    push(bytes) {
      if (opened) enqueue(bytes);
      else held.push(bytes);
    },
    open() {
      // Idempotent without a guard: held is emptied as it is flushed.
      opened = true;
      for (const bytes of held) enqueue(bytes);
      held.length = 0;
    },
  };
}
