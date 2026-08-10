import { describe, expect, it } from "vitest";
import App from "./App";

describe("App", () => {
  it("is a component (scaffold smoke: the real render tests ride PR3+)", () => {
    expect(typeof App).toBe("function");
    expect(App.name).toBe("App");
  });
});
