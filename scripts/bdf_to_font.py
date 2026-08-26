#!/usr/bin/env python3
"""BDF -> param_pages glyph table.

Emits the encoding font4x5.mjs uses ([advance, yOff, w, h, ...rowBits], bit0 =
leftmost pixel) so a generated font drops into the same blitter. Rows with no
ink anywhere in the character set are trimmed, so a 12-row BDF box becomes a
9-row font and the label band only pays for pixels that exist.
"""
import re, sys

CHARS = " !\"'()+,-./:0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%<>=?*#&_\\^"

def parse(path):
    txt = open(path, encoding="latin-1").read(); out = {}
    for m in re.finditer(r"STARTCHAR ([^\n]*)\nENCODING (\d+)\n(.*?)ENDCHAR", txt, re.S):
        enc = int(m.group(2)); blk = m.group(3)
        bb = re.search(r"BBX (-?\d+) (-?\d+) (-?\d+) (-?\d+)", blk)
        dw = re.search(r"DWIDTH (-?\d+) (-?\d+)", blk)
        bm = re.search(r"BITMAP\n(.*)", blk, re.S)
        if not (bb and bm): continue
        w, h, xo, yo = map(int, bb.groups())
        rows = [r.strip() for r in bm.group(1).strip().split("\n") if r.strip()]
        bits = [[1 if (int(r, 16) >> (len(r) * 4 - 1 - i)) & 1 else 0 for i in range(w)] for r in rows]
        out[enc] = dict(w=w, h=h, xo=xo, yo=yo, bits=bits, adv=int(dw.group(1)) if dw else w)
    return out

def pixels(d):
    """absolute (x, yFromTopOfBox) -> set, with yo applied"""
    s = set()
    for i, row in enumerate(d["bits"]):
        for x, v in enumerate(row):
            if v: s.add((x + d["xo"], i - d["yo"]))
    return s


# Glyphs that do not fit the CAP window, hand-drawn to fit it.
#
# The trim below clips every glyph to the window measured on H, and a Tamzen
# 6x12 has plenty of ink outside that: an audit of the 60-character set found
# FOURTEEN glyphs losing pixels, not just the Q this script originally knew
# about. Most matter — measured across the 76-module fleet, "_" appears 9805
# times (and, sitting entirely BELOW the window, was rendering as nothing at
# all), "%" 660, "/" 458, "&" 93, "#" 56, "()" 35.
#
# Two repairs, because there are two failure modes:
#
#   * A glyph SHORTER than the window that merely sits outside it is moved in
#     (handled automatically below) — "_", "'", "," and friends.
#   * A glyph TALLER than the window cannot be moved, only redrawn. Those are
#     here, as 5-wide bitmaps at the window height, drawn to read at this size
#     rather than to reproduce Tamzen exactly. Widening the window is not an
#     option: 7 rows is the equilibrium the whole grid is cut around, and the
#     enum square sets that, not the label.
#
# Rows are top-to-bottom strings, "#" is ink. Anything not listed keeps its
# Tamzen shape.
OVERRIDES = {
    "%": ["##..#",
          "##.#.",
          "...#.",
          "..#..",
          ".#...",
          ".#.##",
          "#..##"],
    "/": ["....#",
          "....#",
          "...#.",
          "..#..",
          ".#...",
          "#....",
          "#...."],
    "\\": ["#....",
           "#....",
           ".#...",
           "..#..",
           "...#.",
           "....#",
           "....#"],
    "(": ["..##.",
          ".#...",
          ".#...",
          ".#...",
          ".#...",
          ".#...",
          "..##."],
    ")": [".##..",
          "...#.",
          "...#.",
          "...#.",
          "...#.",
          "...#.",
          ".##.."],
    "#": [".#.#.",
          ".#.#.",
          "#####",
          ".#.#.",
          "#####",
          ".#.#.",
          ".#.#."],
    "&": [".##..",
          "#..#.",
          "#.#..",
          ".#...",
          "#.#.#",
          "#..#.",
          ".##.#"],
    "!": ["..#..",
          "..#..",
          "..#..",
          "..#..",
          "..#..",
          ".....",
          "..#.."],
    "?": [".###.",
          "#...#",
          "....#",
          "...#.",
          "..#..",
          ".....",
          "..#.."],
}


def override_pixels(rows, top):
    """Hand-drawn rows -> the same pixel set the BDF path produces."""
    out = set()
    for dy, row in enumerate(rows):
        for dx, ch in enumerate(row):
            if ch == "#":
                out.add((dx, top + dy))
    return out


def main(path, tag, chars=None):
    # A restricted charset is not an optimisation, it is a CORRECTNESS bound.
    #
    # OVERRIDES below are hand-drawn at SEVEN rows, because that is the window
    # the 6x12 cut settled on and those glyphs are taller than it. Generate a
    # taller font and every one of them is wrong by construction -- silently, in
    # nine characters nobody would think to look at. Passing the exact charset a
    # caller needs keeps the generated file honest: it contains what it has been
    # cut for, and `missingGlyphs` reports anything else instead of drawing it
    # badly. The number font uses "0123456789+-" and none of those is overridden.
    global CHARS
    if chars:
        CHARS = chars
        bad = sorted(set(chars) & set(OVERRIDES))
        if bad:
            sys.exit("refusing: %s are hand-drawn at 7 rows and cannot be "
                     "generated at another height -- redraw them first" % bad)
    g = parse(path)
    have = {c: pixels(g[ord(c)]) for c in CHARS if ord(c) in g}
    # Trim to the CAP window (measured on H), not to every glyph. Only Q
    # descends below it, and a descender is fatal here: the label band draws
    # the touched value INVERTED, so any ink outside the band is black on
    # black and simply disappears — "FRQ" would read as "FRO". An all-caps UI
    # font wants a non-descending Q anyway, so Q is redrawn to close inside
    # the bowl.
    href = have.get("H") or next(iter(have.values()))
    top = min(y for (_, y) in href)
    bot = max(y for (_, y) in href)
    if "Q" in have:
        o = have.get("O")
        if o:
            q = set(o)
            h = bot - top + 1
            # tail inside the bowl: a diagonal off the lower-right of the O
            xs = sorted({x for (x, _) in o})
            midx = xs[len(xs) // 2]
            q.add((midx, top + h - 3))
            q.add((midx + 1, top + h - 2))
            have["Q"] = q
    # A glyph that FITS the window but sits outside it is moved in rather than
    # clipped. "_" is the whole reason: it lives on row 10 with the window at
    # 2..8, so every one of its pixels was being discarded and it rendered as
    # blank space 9805 times across the fleet.
    for c in list(have):
        ink = have[c]
        if not ink:
            continue
        lo = min(y for (_, y) in ink)
        hi = max(y for (_, y) in ink)
        if lo >= top and hi <= bot:
            continue
        if hi - lo <= bot - top:
            shift = 0
            if lo < top:
                shift = top - lo
            elif hi > bot:
                shift = bot - hi
            have[c] = {(x, y + shift) for (x, y) in ink}

    for c, rows in OVERRIDES.items():
        if c in have:
            have[c] = override_pixels(rows, top)

    for c in list(have):
        have[c] = {(x, y) for (x, y) in have[c] if top <= y <= bot}
    height = bot - top + 1
    lines = []
    for c in CHARS:
        if c not in have:
            lines.append("    [%d, 0, 0, %d]," % (5, height)); continue
        s = have[c]; d = g[ord(c)]
        w = max((x for (x, _) in s), default=-1) + 1
        vals = []
        for y in range(top, bot + 1):
            v = 0
            for x in range(max(w, 1)):
                if (x, y) in s: v |= (1 << x)
            vals.append(v)
        esc = c.replace("\\", "\\\\")
        lines.append("    [%d, 0, %d, %d, %s],  /* %s */" % (d["adv"], max(w, 0), height, ", ".join(map(str, vals)), esc))
    src = '''/**
 * font_%s.mjs — GENERATED by scripts/bdf_to_font.py from %s
 *
 * Tamzen (a cleaned-up Tamsyn), vendored in fonts/tamzen/. Same glyph
 * encoding as font4x5.mjs, so it blits through the same code.
 *
 * Cap height %d rows, advance %d px.
 */

const CHARS = %s;
const G = [
%s
];
const FALLBACK_ADV = %d;

function glyphFor(ch) { const i = CHARS.indexOf(ch); return i >= 0 ? G[i] : null; }

export const HEIGHT = %d;

export function fontWidth(str) {
    let w = 0; const s = String(str == null ? "" : str);
    for (let i = 0; i < s.length; i++) { const g = glyphFor(s[i]); w += g ? g[0] : FALLBACK_ADV; }
    return w > 0 ? w - 1 : 0;
}

export function fontPrint(ctx, x, y, str, color) {
    let cx = x; const s = String(str == null ? "" : str);
    for (let i = 0; i < s.length; i++) {
        const g = glyphFor(s[i]);
        if (!g) { cx += FALLBACK_ADV; continue; }
        const yOff = g[1], w = g[2], h = g[3];
        for (let row = 0; row < h; row++) {
            const bits = g[4 + row];
            if (!bits) continue;
            let col = 0;
            while (col < w) {
                if (bits & (1 << col)) {
                    const start = col;
                    while (col < w && (bits & (1 << col))) col++;
                    ctx.fillRect(cx + start, y + yOff + row, col - start, 1, color);
                } else col++;
            }
        }
        cx += g[0];
    }
}

export function missingGlyphs(str) {
    const out = new Set();
    for (const ch of String(str == null ? "" : str)) if (glyphFor(ch) === null) out.add(ch);
    return out;
}

export const MEASURE = { textWidth: fontWidth };
''' % (tag, path, height, max(d["adv"] for d in g.values()),
       repr(CHARS).replace('"', '\\"'), "\n".join(lines),
       max(d["adv"] for d in g.values()), height)
    open("src/shared/param_pages/font_%s.mjs" % tag, "w").write(src)
    print("wrote src/shared/param_pages/font_%s.mjs  height=%d" % (tag, height))

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
