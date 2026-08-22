#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A modulated knob must never render identically to an unmodulated one.
#
# The modulation dot used to be suppressed when the live value was within 0.02
# of the base, on the reasoning that a dot under the pointer only thickens it.
# True about the pixels, wrong about the meaning: with the dot gone the knob is
# pixel-identical to a parameter nothing is driving, so it does not read as
# "the LFO is at its base", it reads as "there is no LFO".
#
# Reported from the device: a BIPOLAR LFO on a knob at 0 is clamped at 0 for
# half its cycle, so the indicator vanished for half of every cycle.
#
# Asserted on the KNOB region only. The label keeps a tilde either way, and
# including it would let this pass on four pixels a row below the thing being
# looked at.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./tools/param-pages/harness.mjs"),
  import("./src/shared/param_pages/render_page_movy.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
]).then(([H, R, M]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  const cp = [{ key: "k", name: "Depth", type: "float", min: 0, max: 1, step: 0.01 }];
  const ix = M.buildMetaIndex({ chainParams: cp });
  const page = { kind: "knobs", name: "P", level: "root", keys: ["k"] };

  /* Signature of the WIDGET band only -- ROW0_Y..ROW0_Y+BOX_H -- so the
   * label tilde a row below cannot carry this assertion. */
  const knobSig = (value, modValue, isMod) => {
    const fb = H.createFramebuffer();
    R.renderPageMovy(H.drawContext(fb), {
      page, metaIndex: ix, values: { k: value },
      modValues: modValue === null ? null : { k: modValue },
      modulated: () => isMod, pageIndex: 0, pageCount: 1, header: "L",
    });
    const px = fb.pixels || fb.px;
    let h = 0;
    for (let y = R.ROW0_Y; y < R.ROW0_Y + R.BOX_H; y++)
      for (let x = 0; x < R.W; x++) if (px[y * R.W + x]) h = (h * 31 + (y * R.W + x)) | 0;
    return h;
  };

  const unmodulated = knobSig("0", null, false);

  /* Pinned exactly at the base -- the reported case. */
  if (knobSig("0", "0", true) === unmodulated)
    fail("a modulated knob pinned at its base renders EXACTLY like an unmodulated " +
         "one -- there is no way to tell the LFO exists");

  /* And across the range, including the top, where a bipolar source clamps
   * just as it does at the bottom. */
  for (const at of ["0", "0.5", "1"]) {
    const un = knobSig(at, null, false);
    if (knobSig(at, at, true) === un)
      fail("at " + at + ", a modulated knob sitting on its base is indistinguishable " +
           "from an unmodulated one");
  }

  /* The dot must still MOVE when the source moves -- suppressing the old
   * threshold must not have flattened it to a decoration. */
  if (knobSig("0.5", "0.5", true) === knobSig("0.5", "0.9", true))
    fail("the dot does not move with the modulated value");

  console.log("  ok  a modulated knob is distinguishable at base, mid and top");
  console.log("  ok  the dot still tracks the live value");
  console.log("PASS: modulation stays visible when the source lands on the base");
});
'
