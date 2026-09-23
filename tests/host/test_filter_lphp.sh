#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# LP>HP IS TWO CORNERS, NOT A BANDPASS.
#
# M8's LP>HP is a lowpass whose RESONANCE knob sets the corner of a one-pole
# highpass, so the two controls are two independent corners and the filter has
# no resonance at all.
#
# filterModeOf used to read it as a bandpass, on the reasonable-looking rule
# that a name containing both "lp" and "hp" is one. The bandpass is wrong in
# three separate ways at once: a single hump centred on the LOWPASS corner,
# narrowing as the second corner rises instead of moving with it, and carrying
# a resonant peak the filter does not have.
#
# The assertions below are about the SHAPE, not about the mode string, because
# a mode that resolved correctly and drew the wrong curve would pass a string
# check.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the filter tests" >&2
  exit 1
fi

node --input-type=module -e '
import { filterGainAt, drawFilter } from "./src/shared/param_pages/viz_draw.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* Lowpass corner high, highpass corner a third of the way up. */
const C = 0.8, R = 0.35;
const at = (u, mode) => filterGainAt(u, mode, C, R);

ok(at(0.05, "lphp") < 0.05, "below the highpass corner is stopped");
ok(at(0.5, "lphp") > 0.5, "between the corners passes");
ok(at(0.98, "lphp") < 0.05, "above the lowpass corner is stopped");

/* No resonance: the passband is FLAT, and raising the second corner moves
 * the lower edge rather than growing a peak. */
const mid = [0.45, 0.5, 0.55].map((u) => at(u, "lphp"));
ok(Math.max(...mid) - Math.min(...mid) < 0.001, "the passband is flat, with no peak");
ok(filterGainAt(0.5, "lphp", C, 0.1) === filterGainAt(0.5, "lphp", C, 0.2),
   "the second corner does not change the passband level");
/* 0.45 is inside the passband while the second corner sits low, and
 * below it once it is raised. Probing further down finds zero either
 * way, which would pass this for the wrong reason. */
ok(at(0.45, "lphp") > 0.5 && filterGainAt(0.45, "lphp", C, 0.6) < 0.05,
   "raising the second corner closes the band from below");

/* Closing one corner past the other shuts the filter, as it does in reality. */
ok(filterGainAt(0.5, "lphp", 0.2, 0.9) < 0.05, "corners crossed means nothing passes");

/* It must not be the bandpass, nor the plain lowpass it would fall back to. */
const probe = [0.05, 0.2, 0.5, 0.8];
const sig = (mode) => probe.map((u) => at(u, mode).toFixed(3)).join(",");
ok(sig("lphp") !== sig("bp"), "lphp is not the bandpass");
ok(sig("lphp") !== sig("lp"), "lphp is not a plain lowpass");

/* And the name resolves - through drawFilter, since filterModeOf is private. */
function render(typeName) {
  const chainParams = [
    { key: "cut", name: "Cutoff", type: "int", min: 0, max: 127, step: 1 },
    { key: "res", name: "Res", type: "int", min: 0, max: 127, step: 1 },
    { key: "mode", name: "Type", type: "enum", options: [typeName] },
  ];
  const metaIndex = buildMetaIndex({ chainParams });
  const px = [];
  const ctx = {
    fillRect: (x, y, w, h) => px.push(x + "," + y + "," + w + "," + h),
    print: () => {}, textWidth: (t) => String(t).length * 6,
  };
  drawFilter(ctx, { x: 0, y: 0, w: 96, h: 26 },
             { cutoff: "cut", resonance: "res", mode: "mode" },
             { cut: 100, res: 45, mode: 0 }, metaIndex);
  return px.join(";");
}
ok(render("LP>HP") !== render("BANDPASS"), "the name LP>HP does not draw a bandpass");
ok(render("LP>HP") !== render("LOWPASS"), "the name LP>HP does not draw a lowpass");
ok(render("LP > HP") === render("LP>HP"), "spacing around the arrow does not matter");
ok(render("BANDPASS") !== render("LOWPASS"), "the other modes still differ from each other");

process.exit(fail ? 1 : 0);
'
