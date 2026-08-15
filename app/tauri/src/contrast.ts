// WCAG 2.1 contrast, so the palette's claims are computed rather than
// asserted in a comment. theme.test.ts runs every token pair through
// this; a token edit that pushes an ink role under its floor fails there
// instead of shipping as unreadable text.

/** Relative luminance of a #RGB or #RRGGBB color (WCAG 2.1 §relative
 * luminance). Throws on anything else — a silently-accepted bad string
 * would make the ratio meaningless, and every caller here is a literal. */
export function relativeLuminance(hex: string): number {
  const m = /^#([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(hex.trim());
  if (m === null) throw new Error(`not a hex color: ${hex}`);
  const h = m[1].length === 3 ? [...m[1]].map((c) => c + c).join("") : m[1];
  const [r, g, b] = [0, 2, 4].map((i) => {
    const v = parseInt(h.slice(i, i + 2), 16) / 255;
    return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/** Contrast ratio between two colors, 1..21, order-independent. */
export function contrastRatio(a: string, b: string): number {
  const [hi, lo] = [relativeLuminance(a), relativeLuminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}
