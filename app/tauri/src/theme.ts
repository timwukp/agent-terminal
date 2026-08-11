// One dark surface system for the whole shell (direction: Kiro Crew's
// dashboard — dark chrome, hairline borders, one restrained accent).
// Before this the sidebars were unstyled-light around a dark terminal;
// the seam read as two different apps.
//
// The accent doubles as the single chart series color; it passes the
// dataviz palette validator on both this dark surface and light
// (lightness band, chroma, ≥3:1 contrast — scripts/validate_palette.js).

export const theme = {
  /** App chrome: sidebars, panels. */
  bg: "#14171c",
  /** Terminal region backdrop (pre-existing; letterbox shows this). */
  bgMain: "#1e2228",
  /** Raised rows/cards inside chrome. */
  surface: "#1f242c",
  border: "#2d333d",
  text: "#dfe4ea",
  textMuted: "#8b949e",
  accent: "#2b6cb0",
  good: "#3fb950",
  danger: "#e5534b",
} as const;
