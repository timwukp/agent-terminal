import { describe, expect, it } from "vitest";
import { nextSessionName } from "./api";

describe("nextSessionName", () => {
  it("bare prefix when free", () => {
    expect(nextSessionName("claude", [])).toBe("claude");
    expect(nextSessionName("claude", ["shell", "work"])).toBe("claude");
  });
  it("numbers from 2 when taken", () => {
    expect(nextSessionName("claude", ["claude"])).toBe("claude-2");
    expect(nextSessionName("claude", ["claude", "claude-2"])).toBe("claude-3");
  });
  it("fills gaps rather than counting past them", () => {
    // claude-2 died and vanished from ls; its name is free again.
    expect(nextSessionName("claude", ["claude", "claude-3"])).toBe("claude-2");
  });
});
