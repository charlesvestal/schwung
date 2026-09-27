#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# AN ENVELOPE WITH A MODE SWITCH DRAWS THE SHAPE THE VOICE PLAYS.
#
# DR32's pads switch one set of knobs between A-H-D (a timed hold at the peak,
# then decay to silence) and A-S-R (full level while the pad is held, then the
# Decay knob is the release; Hold is ignored). A `mode` role on the envelope
# group -- declared span:false, so the switch keeps its own cell -- picks the
# shape. Pixels, not a grep:
#   - A-H-D: turning Hold moves the picture, and a hold plateau sits AT THE TOP
#   - A-S-R: turning Hold moves NOTHING, and the picture holds a full-level
#     plateau before its fall
#   - the two modes differ, and an envelope with no mode role is unchanged
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

node --input-type=module -e '
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import { renderPageMovy } from "./src/shared/param_pages/render_page_movy.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { resolveViz } from "./src/shared/param_pages/viz.mjs";
import { envelopeModeOf } from "./src/shared/param_pages/viz_draw.mjs";

let fails = 0;
const ok = (c, m) => { if (!c) { console.log("FAIL: " + m); fails++; } else console.log("ok   " + m); };

ok(envelopeModeOf("A-H-D") === "ahd" && envelopeModeOf("A-S-R") === "asr", "the option names resolve");
ok(envelopeModeOf("ASR") === "asr" && envelopeModeOf("Gate") === "asr", "...in other spellings too");
ok(envelopeModeOf("Exp") === null, "an unknown option is no mode, not a guess");

/* DR32 pad_shape, as declared: attack, decay, hold in the amp group; the
 * switch lends its value without joining the span. */
function page(withMode) {
  const cp = [
    { key: "attack", name: "Attack", type: "float", min: 0, max: 20, viz: { group: "amp", role: "attack" } },
    { key: "decay", name: "Decay", type: "float", min: 0, max: 60, viz: { group: "amp", role: "decay" } },
    { key: "hold", name: "Hold", type: "float", min: 0, max: 60, viz: { group: "amp", role: "hold" } },
    { key: "env_mode", name: "Envelope", type: "enum", options: ["A-H-D", "A-S-R"],
      ...(withMode ? { viz: { group: "amp", role: "mode", span: false } } : {}) },
  ];
  const keys = cp.map((p) => p.key);
  const metaIndex = buildMetaIndex({ chainParams: cp });
  return { page: { title: "Shape", kind: "PAGE_KNOBS", keys }, metaIndex, viz: (resolveViz({ keys, metaIndex }) || {}).groups || [] };
}
function shot(withMode, mode, hold) {
  const p = page(withMode);
  const fb = createFramebuffer();
  renderPageMovy(drawContext(fb), { ...p, values: { attack: 2, decay: 20, hold, env_mode: mode },
                                    rect: { x: 0, y: 0, w: 128, h: 64 } });
  return fb;
}
const rows = (fb, y0, y1, x1) => fb.toAscii().split("\n").slice(y0, y1).map((r) => r.slice(0, x1));

const g = page(true).viz.find((x) => x.kind === "envelope");
ok(!!g, "the declared group is an envelope");
ok(g && g.roles.mode === "env_mode", "...carrying the mode role");
ok(g && g.keys.indexOf("env_mode") < 0, "...without claiming the switch cell");

const ahdShort = shot(true, 0, 5).toAscii(), ahdLong = shot(true, 0, 40).toAscii();
const asrShort = shot(true, 1, 5).toAscii(), asrLong = shot(true, 1, 40).toAscii();
ok(ahdShort !== ahdLong, "A-H-D: turning Hold moves the picture");
ok(asrShort === asrLong, "A-S-R: turning Hold moves nothing (the voice ignores it)");
ok(ahdLong !== asrLong, "the two modes draw different shapes");
/* The picture only: the switch cell itself prints its own option. */
const pic = (fb) => rows(fb, 8, 26, 96).join("\n");
ok(pic(shot(false, 1, 40)) === pic(shot(false, 0, 40)),
   "control: with no mode role the switch does not change the picture");
ok(pic(shot(false, 0, 5)) !== pic(shot(false, 0, 40)),
   "control: with no mode role Hold still draws, as it did before");

if (process.env.SHOW) for (const [n, f] of [["A-H-D hold 40", shot(true, 0, 40)], ["A-S-R", shot(true, 1, 40)]])
  console.log("=== " + n + "\n" + rows(f, 8, 26, 100).join("\n"));
if (fails) { console.log(fails + " failure(s)"); process.exit(1); }
console.log("PASS: an envelope mode role draws A-H-D and A-S-R as the voice plays them");
'
