#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A GRAPHIC ON A HELD STEP DRAWS THE LOCK, NOT THE LIVE VALUE.
#
# Both renderers hand their graphics `values` with `modValues` merged over it,
# and for a p-locked key `values` already carries the lock (the controller's
# decoratedValues). A sequencer lane is reported as modulated, so the key is in
# `modValues` too -- and the merge put the value the lane is driving NOW back
# over the step's lock. Turning the knob on a held step moved the knob's mark
# and left the envelope/filter picture where it was. Reported from the device
# as "the visualisation for automated parameters is not updated according to
# the new value".
#
# Pixels, not a grep. The LOCK is held fixed and only the live value varies:
# if the lock drives the picture the two frames are identical. Varying the lock
# instead also changes the label band (a locked band prints the value), which
# differs with the bug in place and measures nothing. The control proves the
# live value does move an UNLOCKED graphic, so identity is not a dead render.

node --input-type=module -e '
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import { renderPageMovy } from "./src/shared/param_pages/render_page_movy.mjs";
import { renderPage } from "./src/shared/param_pages/render_page.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { resolveViz } from "./src/shared/param_pages/viz.mjs";

let fails = 0;
const check = (c, msg) => { if (!c) { console.log("FAIL: " + msg); fails++; } };

const keys = ["attack", "decay", "sustain", "release"];
const page = { title: "Env", kind: "PAGE_KNOBS", keys };
const metaIndex = buildMetaIndex({
  chainParams: keys.map((k) => ({ key: k, name: k[0].toUpperCase() + k.slice(1),
                                  type: "float", min: 0, max: 1, step: 0.01 })),
});
const viz = (resolveViz({ keys, metaIndex }) || {}).groups || [];
/* The premise: without a graphic over the cell this measures a knob, whose
 * mark already honours the lock, and passes with the bug in place. */
if (!viz.some((g) => g.slotStart <= 1 && 1 < g.slotStart + g.slotSpan)) {
  console.log("FAIL: no graphic covers `decay` -- this test would measure a knob");
  process.exit(1);
}

function shot(render, lock, live) {
  const fb = createFramebuffer();
  const dec = lock === null ? null
    : [null, { locked: true, value: lock, exact: false }, null, null, null, null, null, null];
  /* `values` as the controller hands it: the lock folded in (decoratedValues). */
  const values = { attack: 0.2, decay: lock === null ? 0.5 : lock, sustain: 0.5, release: 0.3 };
  render(drawContext(fb), {
    page, metaIndex, values, viz, decorations: dec,
    modValues: { decay: live },           /* what the lane is driving right now */
    rect: { x: 0, y: 0, w: 128, h: 64 },
  });
  return fb.toAscii();
}

for (const [name, render] of [["movy", renderPageMovy], ["dial", renderPage]]) {
  check(shot(render, null, 0.1) !== shot(render, null, 0.9),
        name + ": control -- the live value does not move an unlocked graphic, so " +
        "the assertion below would pass on a dead render");
  check(shot(render, 0.6, 0.1) === shot(render, 0.6, 0.9),
        name + ": the graphic drew the live value over the held step`s lock");
}

if (fails) { console.log(fails + " failure(s)"); process.exit(1); }
console.log("PASS: a held step`s lock drives the graphic, in both renderers");
'
