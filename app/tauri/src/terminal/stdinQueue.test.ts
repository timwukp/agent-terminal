import { describe, expect, it, vi } from "vitest";
import { createStdinQueue } from "./stdinQueue";

const b = (s: string) => new TextEncoder().encode(s);
const seen = (calls: Uint8Array[]) => calls.map((u) => new TextDecoder().decode(u)).join("");

describe("createStdinQueue", () => {
  it("holds keys typed before attach and flushes them in order", async () => {
    const sent: Uint8Array[] = [];
    const q = createStdinQueue(async (x) => void sent.push(x), () => {});

    q.push(b("a"));
    q.push(b("b"));
    // Let any microtask-scheduled send run: asserting synchronously here
    // passes even if push() sends immediately, proving nothing.
    await Promise.resolve();
    await new Promise((r) => setTimeout(r, 0));
    expect(sent).toHaveLength(0); // not attached yet: nothing on the wire

    q.open();
    await vi.waitFor(() => expect(sent).toHaveLength(2));
    expect(seen(sent)).toBe("ab");
  });

  it("preserves order when a flush is still in flight", async () => {
    const sent: Uint8Array[] = [];
    let releaseFirst: (() => void) | null = null;
    const q = createStdinQueue(
      (x) =>
        releaseFirst === null && sent.length === 0
          ? new Promise<void>((r) => {
              releaseFirst = () => {
                sent.push(x);
                r();
              };
            })
          : Promise.resolve(void sent.push(x)),
      () => {},
    );

    q.open();
    q.push(b("1")); // blocks inside send
    q.push(b("2")); // must not overtake "1"

    // The send only starts on a microtask, so wait for it to be in
    // flight before releasing — otherwise this asserts nothing.
    await vi.waitFor(() => expect(releaseFirst).not.toBeNull());
    releaseFirst!();

    await vi.waitFor(() => expect(sent).toHaveLength(2));
    expect(seen(sent)).toBe("12");
  });

  it("reports a failed send instead of swallowing the keystroke", async () => {
    const onError = vi.fn();
    const q = createStdinQueue(() => Promise.reject(new Error("not attached")), onError);
    q.open();
    q.push(b("x"));
    await vi.waitFor(() => expect(onError).toHaveBeenCalledTimes(1));
  });

  it("keeps delivering after one send fails", async () => {
    const sent: Uint8Array[] = [];
    let n = 0;
    const q = createStdinQueue((x) => {
      if (n++ === 0) return Promise.reject(new Error("transient"));
      sent.push(x);
      return Promise.resolve();
    }, () => {});
    q.open();
    q.push(b("lost"));
    q.push(b("kept"));
    await vi.waitFor(() => expect(seen(sent)).toBe("kept"));
  });

  it("open() twice does not resend held input", async () => {
    const sent: Uint8Array[] = [];
    const q = createStdinQueue(async (x) => void sent.push(x), () => {});
    q.push(b("z"));
    q.open();
    q.open();
    await vi.waitFor(() => expect(sent).toHaveLength(1));
  });
});
