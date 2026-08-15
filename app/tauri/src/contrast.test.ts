import { describe, expect, it } from "vitest";
import { contrastRatio, relativeLuminance } from "./contrast";

describe("relativeLuminance", () => {
  it("anchors at the two ends of the scale", () => {
    expect(relativeLuminance("#000000")).toBe(0);
    expect(relativeLuminance("#ffffff")).toBeCloseTo(1, 10);
  });

  it("accepts the #rgb shorthand as the same color", () => {
    expect(relativeLuminance("#fff")).toBe(relativeLuminance("#ffffff"));
    expect(relativeLuminance("#0af")).toBe(relativeLuminance("#00aaff"));
  });

  it("rejects anything that is not a hex color", () => {
    // Silently accepting "rgb(0,0,0)" or "" would make every ratio built
    // on it meaningless — and the ratios are what the palette claims.
    for (const bad of ["", "#12", "#1234", "fff", "rgb(0,0,0)", "#gggggg", "var(--bg)"]) {
      expect(() => relativeLuminance(bad), bad).toThrow(/not a hex color/);
    }
  });
});

describe("contrastRatio", () => {
  it("black on white is the maximum, 21:1", () => {
    expect(contrastRatio("#000", "#fff")).toBeCloseTo(21, 5);
  });

  it("a color against itself is 1:1", () => {
    expect(contrastRatio("#2b6cb0", "#2b6cb0")).toBeCloseTo(1, 10);
  });

  it("is order-independent", () => {
    expect(contrastRatio("#2b6cb0", "#ffffff")).toBeCloseTo(
      contrastRatio("#ffffff", "#2b6cb0"),
      10,
    );
  });

  it("agrees with the published boundary case", () => {
    // #767676 is the canonical lightest gray that still clears 4.5:1 on
    // white; one step lighter does not. An implementation that skipped
    // the sRGB linearization would put this near 3.9.
    expect(contrastRatio("#767676", "#ffffff")).toBeGreaterThanOrEqual(4.5);
    expect(contrastRatio("#777777", "#ffffff")).toBeLessThan(4.5);
  });
});
