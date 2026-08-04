#!/usr/bin/env python3
"""Geometry checker for docs/architecture.svg.

Why this exists: an animated diagram is easy to break silently. Editing one
label's x coordinate can push it under another one, and reading the SVG source
does not reveal it — the first version of this diagram shipped two collisions
that only a render exposed (a label overlapping its neighbour, and a blinking
cursor rect drawn on top of the word "$SHELL").

Two properties are checked, both animation-aware: elements that are never
visible at the same instant are allowed to occupy the same space.

  1. no two <text> nodes that can be co-visible overlap
  2. no decorative <rect>/<circle> is drawn over co-visible glyphs

Presentation attributes are resolved through ancestors, because font-size and
text-anchor are usually set on a parent <g>.

This is a geometric approximation (monospace advance ratio), not a substitute
for looking at a render. It catches regressions; eyeball new artwork.

Usage: python3 tools/check_svg.py docs/architecture.svg
"""
import sys
import xml.etree.ElementTree as ET

NS = "{http://www.w3.org/2000/svg}"
ADVANCE = 0.62   # monospace width/em (Menlo, SF Mono ~0.602) + small margin
SAMPLES = 241    # timeline resolution: 12s / 240 = 50ms

# Decorations whose overlap with text is intentional: pill badges that are
# drawn deliberately *behind* their own label. Keyed by (kind, x, y).
ALLOWED = {("rect", 672.0, 314.0)}   # the SURVIVES badge pill


def _floats(spec):
    return [float(v) for v in spec.split(";")]


def _interp(keytimes, values, frac):
    for i in range(len(keytimes) - 1):
        if keytimes[i] <= frac <= keytimes[i + 1]:
            span = keytimes[i + 1] - keytimes[i]
            if span == 0:
                return values[i + 1]
            r = (frac - keytimes[i]) / span
            return values[i] * (1 - r) + values[i + 1] * r
    return values[-1]


def opacity_at(el, t, total):
    """Opacity of one element at time t, honouring an opacity <animate>."""
    for a in el.findall(NS + "animate"):
        if a.get("attributeName") != "opacity":
            continue
        values = _floats(a.get("values"))
        dur = float(a.get("dur", "0").rstrip("s")) or total
        kt = a.get("keyTimes")
        kt = _floats(kt) if kt else [i / (len(values) - 1) for i in range(len(values))]
        return _interp(kt, values, (t % dur) / dur)
    static = el.get("opacity")
    return float(static) if static else 1.0


def timeline(root):
    """Longest animation duration = one full loop of the diagram."""
    durs = [float(a.get("dur", "0").rstrip("s")) for a in root.iter(NS + "animate")]
    return max(durs) if durs else 1.0


def visible_frames(chain, total):
    """Frame indices where every ancestor of an element is visible."""
    return {
        i for i in range(SAMPLES)
        if all(opacity_at(el, i * total / (SAMPLES - 1), total) > 0.5 for el in chain)
    }


def resolve(root):
    """Yield (element, inherited-attrs, ancestor-chain) for the whole tree."""
    out = []

    def walk(el, inherited, chain):
        cur = dict(inherited)
        for key in ("font-size", "text-anchor", "font-family"):
            if el.get(key):
                cur[key] = el.get(key)
        here = chain + [el]
        out.append((el, cur, here))
        for child in el:
            walk(child, cur, here)

    walk(root, {}, [])
    return out


def glyph_box(el, inherited):
    text = "".join(el.itertext()).strip()
    if not text:
        return None
    size = float(inherited.get("font-size", 13))
    x, y = float(el.get("x", 0)), float(el.get("y", 0))
    width = len(text) * size * ADVANCE
    anchor = inherited.get("text-anchor", "start")
    x0 = x - width if anchor == "end" else x - width / 2 if anchor == "middle" else x
    # baseline y: ascent ~0.75em above, descent ~0.22em below
    return (x0, x0 + width, y - size * 0.75, y + size * 0.22, text, size)


def main(path):
    root = ET.parse(path).getroot()
    total = timeline(root)
    viewbox = [float(v) for v in root.get("viewBox").split()]
    resolved = resolve(root)

    texts, decorations = [], []
    for el, inherited, chain in resolved:
        if el.tag == NS + "text":
            box = glyph_box(el, inherited)
            if box:
                texts.append((box, visible_frames(chain, total)))
        elif el.tag == NS + "rect":
            w, h = float(el.get("width", 0)), float(el.get("height", 0))
            fill = el.get("fill", "none")
            # skip container panels and background-coloured fills
            if fill in ("none", "#0d1117") or w > 200 or h > 60:
                continue
            x, y = float(el.get("x", 0)), float(el.get("y", 0))
            if ("rect", x, y) in ALLOWED:
                continue
            decorations.append((("rect", x, x + w, y, y + h), visible_frames(chain, total)))
        elif el.tag == NS + "circle":
            cx, cy = float(el.get("cx", 0)), float(el.get("cy", 0))
            r = float(el.get("r", 0))
            decorations.append((("circle", cx - r, cx + r, cy - r, cy + r),
                                visible_frames(chain, total)))

    failures = []

    # 1. co-visible text vs text, same baseline
    for i in range(len(texts)):
        for j in range(i + 1, len(texts)):
            (a, va), (b, vb) = texts[i], texts[j]
            if not (va & vb):
                continue
            if abs(a[2] - b[2]) >= max(a[5], b[5]) * 0.9:   # different baselines
                continue
            overlap = min(a[1], b[1]) - max(a[0], b[0])
            if overlap > 1:
                failures.append(f"{overlap:.1f}px text/text: {a[4][:38]!r} <-> {b[4][:38]!r}")

    # 2. co-visible decoration over glyphs
    for (d, vd) in decorations:
        for (t, vt) in texts:
            if not (vd & vt):
                continue
            if d[1] < t[1] - 0.5 and t[0] < d[2] - 0.5 and d[3] < t[3] - 0.5 and t[2] < d[4] - 0.5:
                overlap = min(d[2], t[1]) - max(d[1], t[0])
                failures.append(f"{overlap:.1f}px {d[0]}@x={d[1]:.0f} over text {t[4][:38]!r}")

    # 3. bounds
    for (t, _) in texts:
        if t[0] < viewbox[0] - 1 or t[1] > viewbox[0] + viewbox[2] + 1:
            failures.append(f"out of bounds: {t[4][:40]!r} spans [{t[0]:.0f},{t[1]:.0f}]")

    print(f"{path}: {len(texts)} texts, {len(decorations)} decorations, "
          f"{SAMPLES} samples over {total:g}s")
    if failures:
        print(f"FAIL ({len(failures)}):")
        for f in failures:
            print(f"  {f}")
        return 1
    print("PASS: no co-visible overlaps, nothing out of bounds")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "docs/architecture.svg"))
