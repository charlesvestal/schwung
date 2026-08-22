#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The header bar's two sides share 128px by MEASURING, not by a fixed split.
#
# It used to give the left 55% and the right 60%, which is two bugs at once:
#
#   - 55 + 60 is 115%, so a long page name and a long title OVERLAPPED and drew
#     glyphs through each other;
#   - the right side is usually a page name and those are short (29px at the
#     fleet median), so the title was capped at 70px while a third of the bar
#     sat empty. That is what made an airwindows effect read as
#     "MFX > BRIGHTAMBI" -- the name is 94px and there was 104px of bar. Worse
#     than unreadable: "BrightAmbience" and "BrightAmbience3" truncated to the
#     SAME string, so two different effects displayed identically.
#
# Overlap is asserted on the PIXELS rather than on the arithmetic, because the
# arithmetic is what was wrong.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./tools/param-pages/harness.mjs"),
  import("./src/shared/param_pages/render_page_movy.mjs"),
  import("./src/shared/param_pages/font4x5.mjs"),
]).then(([H, R, F]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /*
   * Split the band at its WIDEST run of blank columns -- that is where the two
   * sides face each other. Merging "contiguous ink" instead splits the title
   * at its own word spaces, which made the first version of this test report
   * a 94px title as 15px ("MFX") and fail for the wrong reason.
   */
  const measure = (left, right) => {
    const fb = H.createFramebuffer();
    R.drawHeader(H.drawContext(fb), left, right, false);
    const px = fb.pixels || fb.px;
    const on = [];
    for (let x = 0; x < R.W; x++) {
      let ink = false;
      for (let y = 0; y < R.HEADER_H; y++) if (px[y * R.W + x]) { ink = true; break; }
      on.push(ink);
    }
    const cols = on.reduce((a, v, i) => (v ? (a.push(i), a) : a), []);
    if (!cols.length) return { leftW: 0, gap: R.W, any: false };
    let best = 0, at = -1;
    for (let i = 1; i < cols.length; i++) {
      const g = cols[i] - cols[i - 1] - 1;
      if (g > best) { best = g; at = i; }
    }
    if (at < 0) return { leftW: cols[cols.length - 1] - cols[0] + 1, gap: R.W, any: true };
    return { leftW: cols[at - 1] - cols[0] + 1, gap: best, any: true };
  };

  /*
   * An ABSOLUTE floor for the gutter, independent of HEADER_GAP.
   *
   * Asserting only "gap >= HEADER_GAP" is self-referential: it reads the
   * constant for both the guarantee and the bar, so lowering the constant
   * lowers the bar and the test stays green. Mutating HEADER_GAP to 3
   * survived exactly that way.
   *
   * 4px is not arbitrary. font4x5 advances 1px between glyphs and its space
   * is 2px, so a gutter of 3 or less is inside the range the eye reads as
   * ordinary word spacing -- the page name would join the end of the title
   * as if it were the last word of it.
   */
  const MIN_LEGIBLE_GUTTER = 4;
  if (R.HEADER_GAP < MIN_LEGIBLE_GUTTER)
    fail("HEADER_GAP is " + R.HEADER_GAP + "px. font4x5 puts 1px between glyphs " +
         "and 2px in a space, so anything under " + MIN_LEGIBLE_GUTTER +
         "px reads as word spacing and the page name joins the title");

  const LONG_PAGE = "PERFORMANCE";
  const CASES = [
    ["MFX > BRIGHTAMBIENCE3", "ROOT"],
    ["MFX > CONSOLE7CHANNEL", "FILTER"],
    ["S1 > SOMETHING VERY LONG INDEED", LONG_PAGE],
    ["MFX > CS", "ROOT"],
    ["S1 > SURGE XT", "AMP_ENV"],
  ];

  /* ---- 1. the two sides keep a MINIMUM GUTTER -----------------------
   *
   * Not merely "do not overlap". HEADER_GAP is guaranteed by construction --
   * the left is fitted to W - 4 - rightWidth - HEADER_GAP and the right is
   * drawn at W - rightWidth - 2 -- so the assertion is the constant itself,
   * measured at 4px across 2160 title/page-name combinations and never less.
   *
   * Pinning ">= 2" instead would pass with the guarantee halved, which is the
   * kind of slack that lets a fixed-fraction split creep back. */
  for (const [l, r] of CASES) {
    const { gap, any } = measure(l, r);
    if (!any) continue;
    if (gap < Math.max(R.HEADER_GAP, MIN_LEGIBLE_GUTTER))
      fail("\"" + l + "\" / \"" + r + "\": the two sides are " + gap +
           "px apart, under the " + R.HEADER_GAP + "px gutter -- they read as " +
           "one string, or overlap");
  }

  /* ---- 2. a SHORT right side hands its room to the title ------------
   *
   * The regression this exists to stop is the fixed split coming back. With a
   * short page name a title that fits the remainder must be drawn WHOLE. */
  {
    const title = "MFX > BRIGHTAMBIENCE3";
    const drawn = measure(title, "ROOT").leftW;
    const want = F.fontWidth4x5(title);
    if (drawn < want - 1)
      fail("with a 4-character page name the title drew " + drawn + "px of " +
           want + "px -- it is still being capped at a fixed fraction");
  }

  /* ---- 3. a LONG right side must not squeeze the title to nothing ---
   *
   * The floor is the old fixed 55%, so the worst case is exactly what
   * shipped before this change. */
  {
    const drawn = measure("S1 > SOMETHING VERY LONG INDEED", LONG_PAGE).leftW;
    if (drawn < R.HEADER_MIN_LEFT - 6)
      fail("a long page name squeezed the title to " + drawn + "px, under the " +
           R.HEADER_MIN_LEFT + "px floor");
  }

  console.log("  ok  the two sides hold their " + R.HEADER_GAP +
              "px gutter, across long/short combinations");
  console.log("  ok  a short page name gives its room to the title (BRIGHTAMBIENCE3 fits whole)");
  console.log("  ok  a long page name cannot squeeze the title below the " +
              R.HEADER_MIN_LEFT + "px floor");
  console.log("PASS: the header splits its bar by measurement");
});
'
