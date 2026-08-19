#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Integrity of the GENERATED label font (src/shared/param_pages/font_tamzen6x12.mjs,
# produced by scripts/bdf_to_font.py from fonts/tamzen/Tamzen6x12r.bdf).
#
# The generator trims every glyph to a CAP window measured on H, and Tamzen
# 6x12 has plenty of ink outside that window. Fourteen of the sixty glyphs were
# losing pixels and nothing noticed, because nothing rendered them: "%" came
# out as a scribble (spotted on hardware), and "_" — which sits ENTIRELY below
# the window — was being discarded down to nothing and drawing as blank space
# 9805 times across the fleet.
#
# A generated font with no test is a font that silently rots the next time the
# window moves. These assertions are deliberately about INK, not exact
# bitmaps: a redraw should be free to look different, but no glyph the UI can
# print may come out empty or clipped to a stub.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the font test" >&2
  exit 1
fi

node -e '
import("./src/shared/param_pages/font_tamzen6x12.mjs").then((F) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* Render one string into a bitmap and hand back the lit pixels. */
  const ink = (s) => {
    const W = s.length * 12 + 8, H = F.HEIGHT + 4;
    const px = new Uint8Array(W * H);
    const ctx = { fillRect: (x, y, w, h, c) => {
      for (let yy = y; yy < y + h; yy++) for (let xx = x; xx < x + w; xx++) {
        if (xx >= 0 && yy >= 0 && xx < W && yy < H) px[yy * W + xx] = c ? 1 : 0;
      }
    } };
    F.fontPrint(ctx, 1, 1, s, 1);
    const pts = [];
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (px[y * W + x]) pts.push([x, y]);
    return pts;
  };

  /* ---- 1. every printable glyph puts ink on the screen ------------------ */
  const CHARS = " !\"#&()+,-./0123456789:<=>?ABCDEFGHIJKLMNOPQRSTUVWXYZ^_*%";
  const blank = [];
  for (const c of CHARS) {
    if (c === " ") continue;
    if (F.fontWidth(c) === 0) continue;   /* not in the set at all */
    if (ink(c).length === 0) blank.push(c);
  }
  if (blank.length) {
    fail("these glyphs render as NOTHING: " + blank.map((c) => JSON.stringify(c)).join(" ")
       + " — a glyph outside the cap window must be moved into it or redrawn, not clipped away");
  }

  /* ---- 2. the ones the fleet actually uses are not stubs ---------------- *
   * Counts measured on the repaired font; the floor catches a glyph clipped
   * back to a fragment without pinning an exact bitmap. */
  const MIN_INK = { "%": 12, "/": 7, "_": 4, "#": 15, "&": 10, "(": 7, ")": 7, "\u0027": 2 };
  for (const c of Object.keys(MIN_INK)) {
    const n = ink(c).length;
    if (n < MIN_INK[c]) {
      fail("glyph " + JSON.stringify(c) + " has only " + n + " lit pixels (expected >= "
         + MIN_INK[c] + ") — it is clipped to a stub");
    }
  }

  /* ---- 3. nothing draws outside the declared glyph box ------------------ *
   * fontPrint is blitted at a fixed row height everywhere it is used, so ink
   * above or below HEIGHT lands in the neighbouring band on the device. */
  for (const c of CHARS) {
    if (c === " " || F.fontWidth(c) === 0) continue;
    for (const [, y] of ink(c)) {
      if (y < 1 || y > F.HEIGHT) {
        fail("glyph " + JSON.stringify(c) + " draws at row " + (y - 1)
           + ", outside its " + F.HEIGHT + "-row box");
      }
    }
  }

  /* ---- 4. no glyph is wider than its advance --------------------------- *
   * A glyph wider than the advance overlaps the next character, which is the
   * easiest way for a hand-drawn replacement to go wrong.
   *
   * fontWidth returns the MEASURED width of a string (advance per glyph less
   * the trailing inter-character gap), not the advance, so derive the advance
   * from the difference between two glyphs and one. Equal is fine and
   * intentional: "_" is full-advance so that consecutive underscores join
   * into a continuous rule. */
  for (const c of CHARS) {
    if (c === " " || F.fontWidth(c) === 0) continue;
    const advance = F.fontWidth(c + c) - F.fontWidth(c);
    const pts = ink(c);
    if (!pts.length || advance <= 0) continue;
    const width = Math.max(...pts.map((p) => p[0]));   /* printed at x=1 */
    if (width > advance) {
      fail("glyph " + JSON.stringify(c) + " draws " + width + "px wide but advances "
         + advance + " — it will collide with the next character");
    }
  }

  console.log("PASS: label font — " + CHARS.length + " glyphs, none blank, none clipped to a stub, "
            + "none drawing outside its box or advance");
});
'
