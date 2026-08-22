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

  const LONG_PAGE = "PERFORMANCE";
  const CASES = [
    ["MFX > BRIGHTAMBIENCE3", "ROOT"],
    ["MFX > CONSOLE7CHANNEL", "FILTER"],
    ["S1 > SOMETHING VERY LONG INDEED", LONG_PAGE],
    ["MFX > CS", "ROOT"],
    ["S1 > SURGE XT", "AMP_ENV"],
  ];

  /* ---- 1. the two sides must never touch ---------------------------- */
  for (const [l, r] of CASES) {
    const { gap, any } = measure(l, r);
    if (!any) continue;
    if (gap < 2)
      fail("\"" + l + "\" / \"" + r + "\": the two sides are " + gap +
           "px apart -- they read as one string, or overlap");
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

  console.log("  ok  the two sides never overlap, across long/short combinations");
  console.log("  ok  a short page name gives its room to the title (BRIGHTAMBIENCE3 fits whole)");
  console.log("  ok  a long page name cannot squeeze the title below the " +
              R.HEADER_MIN_LEFT + "px floor");
  console.log("PASS: the header splits its bar by measurement");
});
'
