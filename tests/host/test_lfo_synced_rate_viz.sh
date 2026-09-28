#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A SYNCED RATE DRAWS AS THE RATE IT PLAYS AT.
#
# The LFO page shows Rate in its own cell now, outside the wave, and the wave
# still reads it through a span:false role. For a division that reading used to
# be the option INDEX -- a position in "16 bar".."1/32T", unrelated to the
# 0.1..20 Hz range the free cell spans -- so turning Sync changed the picture
# for no reason the sound gave. At the nominal 120 BPM, 1/4 plays at 2 Hz and
# must draw exactly as 2 Hz does; a slower division must draw fewer cycles.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const R = process.cwd();
const { drawLfo } = await import(R + "/src/shared/param_pages/viz_draw.mjs");
const { buildMetaIndex } = await import(R + "/src/shared/param_pages/param_meta.mjs");
const LP = await import(R + "/src/shared/param_pages/lfo_page.mjs");

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

const params = LP.lfoParams(1);
const mi = buildMetaIndex({ hierarchy: null, chainParams: params });
function draw(rateKey, rateVal) {
  const px = new Set();
  const put = (x, y) => px.add(Math.round(x) + "," + Math.round(y));
  const ctx = new Proxy({}, { get: (_, name) => (...a) => {
    if (name === "fillRect") { for (let i = 0; i < a[2]; i++) for (let j = 0; j < a[3]; j++) put(a[0] + i, a[1] + j); }
    else if (name === "drawLine" || name === "line") put(a[0], a[1]), put(a[2], a[3]);
    else if (typeof a[0] === "number" && typeof a[1] === "number") put(a[0], a[1]);
  } });
  const roles = { shape: "lfo1:shape", depth: "lfo1:depth", phase: "lfo1:phase_offset",
                  rate: "lfo1:" + rateKey };
  const values = { "lfo1:shape": "0", "lfo1:depth": "1", "lfo1:phase_offset": "0",
                   ["lfo1:" + rateKey]: String(rateVal) };
  drawLfo(ctx, { x: 32, y: 34, w: 96, h: 14 }, roles, values, mi);
  return [...px].sort().join(" ");
}
const idx = (name) => LP.LFO_DIVISIONS.indexOf(name);
if (idx("1/4") < 0) bad("no 1/4 division to test");
const quarter = draw("rate_div", idx("1/4"));
if (quarter !== draw("rate_hz", 2)) bad("a synced 1/4 does not draw as 2 Hz");
if (quarter === draw("rate_hz", 8)) bad("the comparison cannot tell 2 Hz from 8 Hz");
if (draw("rate_div", idx("1/16")) !== draw("rate_hz", 8)) bad("a synced 1/16 does not draw as 8 Hz");

if (fail) process.exit(1);
console.log("PASS: a synced LFO rate draws at the rate it plays (1/4 = 2 Hz at 120 BPM)");
'
