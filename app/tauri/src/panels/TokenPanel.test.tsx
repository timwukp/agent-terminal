// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { act, cleanup, render } from "@testing-library/react";
import TokenPanel from "./TokenPanel";
import { agoText, fmtTokens, type TranscriptUsage, type UsageApi } from "./usageApi";

function transcript(over: Partial<TranscriptUsage> = {}): TranscriptUsage {
  return {
    id: "abcd1234-9999",
    project: "-opt-proj",
    totals: {
      input_tokens: 42,
      output_tokens: 1500,
      cache_read_input_tokens: 2_400_000,
      cache_creation_input_tokens: 981,
    },
    messages: 12,
    malformed: 0,
    model: "claude-fable-5",
    last_timestamp: "2026-08-11T13:42:00.000Z",
    buckets: [
      { minute: "2026-08-11T13:41", output_tokens: 900 },
      { minute: "2026-08-11T13:42", output_tokens: 600 },
    ],
    ...over,
  };
}

function apiOf(rows: TranscriptUsage[]): UsageApi & { calls: number } {
  const api = {
    calls: 0,
    snapshot() {
      api.calls++;
      return Promise.resolve(rows);
    },
  };
  return api;
}

beforeEach(() => vi.useFakeTimers());
afterEach(() => {
  cleanup();
  vi.useRealTimers();
});

describe("TokenPanel", () => {
  it("renders totals as compact text and the newest row's sparkline", async () => {
    const view = render(<TokenPanel api={apiOf([transcript()])} />);
    await act(async () => {});
    // Counters: the accessible table view of the same numbers.
    expect(view.container.textContent).toContain("1.5k"); // out
    expect(view.container.textContent).toContain("2.4M"); // cache read
    expect(view.container.textContent).toContain("abcd1234");
    expect(view.container.textContent).toContain("12 msgs");
    // Single-series sparkline: one rect per minute bucket, no legend.
    expect(view.container.querySelectorAll("svg rect").length).toBe(2);
  });

  it("shows the empty state, not an error, when nothing is active", async () => {
    const view = render(<TokenPanel api={apiOf([])} />);
    await act(async () => {});
    expect(view.container.textContent).toContain("no transcripts active");
    expect(view.queryByRole("alert")).toBeNull();
  });

  it("surfaces unparsed-line counts as a badge (undercount, never silence)", async () => {
    const view = render(<TokenPanel api={apiOf([transcript({ malformed: 3 })])} />);
    await act(async () => {});
    expect(view.container.textContent).toContain("3 unparsed");
  });

  it("polls on the sidebar cadence and stops when unmounted", async () => {
    const api = apiOf([]);
    const view = render(<TokenPanel api={api} />);
    await act(async () => {});
    expect(api.calls).toBe(1);
    await act(async () => {
      vi.advanceTimersByTime(2000);
    });
    expect(api.calls).toBe(2);
    view.unmount();
    await act(async () => {
      vi.advanceTimersByTime(6000);
    });
    expect(api.calls).toBe(2);
  });

  it("shows a fetch failure as an alert instead of a stale blank", async () => {
    const api: UsageApi = { snapshot: () => Promise.reject(new Error("HOME is not set")) };
    const view = render(<TokenPanel api={api} />);
    await act(async () => {});
    expect(view.getByRole("alert").textContent).toContain("HOME is not set");
  });
});

describe("fmtTokens", () => {
  it("keeps small numbers whole and compacts the rest", () => {
    expect(fmtTokens(999)).toBe("999");
    expect(fmtTokens(1500)).toBe("1.5k");
    expect(fmtTokens(2_400_000)).toBe("2.4M");
  });
});

describe("agoText", () => {
  const now = Date.parse("2026-08-11T14:00:00.000Z");
  it("buckets into now/minutes/hours/days", () => {
    expect(agoText("2026-08-11T13:59:30.000Z", now)).toBe("just now");
    expect(agoText("2026-08-11T13:42:00.000Z", now)).toBe("18m ago");
    expect(agoText("2026-08-11T10:00:00.000Z", now)).toBe("4h ago");
    expect(agoText("2026-08-09T10:00:00.000Z", now)).toBe("2d ago");
  });
  it("returns empty for garbage instead of NaN", () => {
    expect(agoText("not a date", now)).toBe("");
  });
});
