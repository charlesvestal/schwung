#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE LOCK MARK MEANS "A POINT SITS HERE", NOT "A VALUE IS BEING SHOWN".
#
# Both renderers already carry that design in a comment -- inversion says "you
# are being shown a value" (touched or locked), the corner says "there is a
# lock here" -- and neither honoured it: `exact` was read off `<key>:held`,
# carried into the decoration, and then never drawn.
#
# The cost is a clear that works and looks like it did not. Clearing a lock
# leaves the knob showing a value (correct: the readout answers the CURVE, and
# a recorded sweep still interpolates across the step whose point you removed)
# and, with the mark still lit, nothing on screen says the lock is gone.
# Reported from the device in exactly those terms.
#
# Pixels, not a grep: this is the second time a flag was honoured by the input
# layer and ignored by the draw layer (see `access: "read"` in CLAUDE.md), and
# a source pin cannot tell the difference.

node --input-type=module -e '
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import { renderPageMovy } from "./src/shared/param_pages/render_page_movy.mjs";
import { renderPage } from "./src/shared/param_pages/render_page.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";

let fails = 0;
const check = (c, msg) => { if (!c) { console.log("FAIL: " + msg); fails++; } };

const page = { title: "Main", kind: "PAGE_KNOBS", keys: ["cutoff", "res"] };
/* The real index, not a hand-rolled object: the renderers call getOrGuess on
 * it, and a stand-in would be a second belief about what a meta is. */
const metaIndex = buildMetaIndex({
  chainParams: [
    { key: "cutoff", name: "Cutoff", type: "float", min: 0, max: 1, step: 0.01 },
    { key: "res",    name: "Res",    type: "float", min: 0, max: 1, step: 0.01 },
  ],
});
const values = { cutoff: 0.5, res: 0.5 };

function shot(render, dec) {
  const fb = createFramebuffer();
  render(drawContext(fb), {
    page, metaIndex, values,
    rect: { x: 0, y: 0, w: 128, h: 64 },
    decorations: [dec, null, null, null, null, null, null, null],
  });
  return fb.rows ? fb.rows().join("\n") : JSON.stringify(fb.pixels || fb.buf || fb);
}

for (const [name, render] of [["movy", renderPageMovy], ["list", renderPage]]) {
  /* A real lock: a point sits on this step. */
  const exact   = shot(render, { locked: true, value: 0.8, exact: true });
  /* The SAME value, but the curve merely passes through -- no point here. */
  const curve   = shot(render, { locked: true, value: 0.8, exact: false });
  /* Nothing held at all. */
  const plain   = shot(render, null);

  check(exact !== curve,
        name + ": a lock and a value the curve passes through render IDENTICALLY -- " +
        "clearing a lock cannot be seen");
  check(curve !== plain,
        name + ": a held value that is not a lock vanished -- it is still what the " +
        "step will play and must be shown");

  /* AND A DECORATION WITHOUT `exact` KEEPS THE OLD MEANING, so a host that
   * never supplied it is unchanged. */
  const legacy = shot(render, { locked: true, value: 0.8 });
  check(legacy === exact,
        name + ": a decoration carrying no `exact` changed meaning -- older hosts " +
        "would silently lose their lock mark");
}

if (fails) { console.log(fails + " failure(s)"); process.exit(1); }
console.log("PASS: the lock mark follows `exact`, in both renderers");
'
