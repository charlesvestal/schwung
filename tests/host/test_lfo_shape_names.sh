#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# AN UNRECOGNISED LFO SHAPE NAME DRAWS A SINE, SILENTLY.
#
# lfoShapeIdOf matches whole words and ends in `return 0`, so a name it does
# not know is not an error -- it is a sine. That is the worst possible
# failure mode for a picture of a waveform: it looks deliberate.
#
# M8 spells its ten shapes TRI, SIN, RAMP DN, RAMP UP, EXP DN, EXP UP, SQU DN,
# SQU UP, RANDOM, DRUNK. Five of those used to fall through -- RAMP DN missed
# `rampdown` by two letters, SQU DN missed `squ` by three, and there was no
# exponential or second square phase in the table at all.
#
# The assertion that matters is that each name draws something DIFFERENT from
# a sine and from its own opposite. Checking only that the call returns would
# pass for the bug being guarded against.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the LFO shape tests" >&2
  exit 1
fi

node --input-type=module -e '
import { lfoShapeSample, drawLfo } from "./src/shared/param_pages/viz_draw.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* Draw one LFO group whose shape enum is sitting on `shape`, and return the
 * pixels as a string. Going through drawLfo rather than lfoShapeSample is
 * deliberate: lfoShapeIdOf is private, and the name lookup is the half that
 * was broken. */
function render(shape) {
  const chainParams = [
    { key: "sh", name: "Shape", type: "enum", options: [shape] },
    { key: "rate", name: "Rate", type: "int", min: 0, max: 127, step: 1 },
    { key: "depth", name: "Depth", type: "int", min: 0, max: 127, step: 1 },
  ];
  const metaIndex = buildMetaIndex({ chainParams });
  const px = [];
  const ctx = {
    fillRect: (x, y, w, h) => px.push(x + "," + y + "," + w + "," + h),
    print: () => {},
    textWidth: (t) => String(t).length * 6,
  };
  drawLfo(ctx, { x: 0, y: 0, w: 96, h: 26 },
          { shape: "sh", rate: "rate", depth: "depth" },
          { sh: 0, rate: 90, depth: 127 }, metaIndex);
  return px.join(";");
}

const sine = render("SIN");
ok(sine.length > 0, "a sine actually draws something");

/* Every M8 name must resolve to a wave of its own. */
for (const name of ["TRI", "RAMP DN", "RAMP UP", "EXP DN", "EXP UP",
                    "SQU DN", "SQU UP", "RANDOM", "DRUNK"]) {
  ok(render(name) !== sine, name + " does not fall through to a sine");
}

/* ...and opposites must actually be opposite, which a shared fallback
 * would hide. */
ok(render("RAMP DN") !== render("RAMP UP"), "RAMP DN and RAMP UP differ");
ok(render("EXP DN") !== render("EXP UP"), "EXP DN and EXP UP differ");
ok(render("SQU DN") !== render("SQU UP"), "SQU DN and SQU UP differ");

/* The long spellings keep working alongside the abbreviations. */
ok(render("RAMP DOWN") === render("RAMP DN"), "RAMP DOWN and RAMP DN agree");
ok(render("SQUARE") === render("SQU DN"), "SQUARE is the high-first square, as SQU DN is");

/* The waveforms themselves: an exponential starts hard against one rail
 * and flattens toward the other, which is what separates it from a ramp. */
const expDn = [0, 0.5, 1].map((t) => lfoShapeSample(100, t));
ok(expDn[0] > 0.9, "EXP DN starts at the top");
ok(expDn[1] < -0.5, "EXP DN is already most of the way down at the halfway point");
const ramp = [0, 0.5].map((t) => lfoShapeSample(6, t));
ok(Math.abs(ramp[1]) < 0.01, "a ramp down is only halfway at the halfway point");

ok(lfoShapeSample(3, 0.1) === 1 && lfoShapeSample(102, 0.1) === -1,
   "the two square phases are inverses");

process.exit(fail ? 1 : 0);
'
