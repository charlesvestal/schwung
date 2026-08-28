#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A VALUE ARRIVING IS NOT A VALUE CHANGING.
#
# The read cursor serves ONE key per tick, so a full page of 8 knobs takes ~9
# ticks (~200ms) to populate. Until a key`s read lands, `s.values[key]` is
# undefined -- and every animated widget rendered that absence as a CONCRETE
# PLACEHOLDER: drawSwitch read NaN and drew OFF, drawWaveform resolved shape 0,
# drawEnumSquare sized itself around "--". The store recorded that placeholder
# as the settled first sighting, and the real value then arrived as a CHANGE.
#
# So every page animated itself in on entry -- switches sweeping on, waveforms
# morphing, enum boxes growing -- from values nobody had set. This is the
# tri-state read rule one layer below where it is usually enforced: an
# unanswered read must not become a picture, and here it became a picture that
# then moved.
#
# THE FIX IS NOT "DO NOT ANIMATE". This asserts BOTH halves:
#   - an arrival draws the settled frame immediately, and
#   - a change after that still animates.
# A test with only the first half passes with every animation deleted.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the anim absence test" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, LAYOUT_MOVY }
  from "./src/shared/param_pages/page_controller.mjs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* One of each animated widget: the waveform morph and the enum square resize.
   A test carrying only one of them passes while the other still animates
   itself in.

   THE SWITCH IS DELIBERATELY NOT A SUBJECT. It used to be -- it was the
   loudest of the three -- but #323 cut its 160ms fill entirely ("the switch
   toggles, it does not animate"), so it can no longer prove anything about an
   arrival. `onoff` stays in the fixture only to keep a switch on the page, so
   that a future change re-animating it is caught by the frame comparison
   rather than sailing past a test that stopped looking. */
const CHAIN_PARAMS = [
  { key: "onoff", name: "Gate", type: "enum", options: ["Off", "On"] },
  { key: "shape", name: "Shape", type: "enum",
    options: ["Sine", "Tri", "Saw", "Square", "Noise"] },
  { key: "mode", name: "Mode", type: "enum",
    options: ["A", "Beta", "Gamma"] },
];
const HIER = { modes: null, levels: { root: { label: "T",
  knobs: ["onoff", "shape", "mode"], params: CHAIN_PARAMS.map((p) => ({ key: p.key })) } } };

let clock = 1000;
/* The values the module WILL report once it answers. Deliberately not the
   zeroth option of anything: the placeholder every widget falls back to is
   option 0, so a fixture sitting on 0 cannot tell an arrival from a no-op. */
const store = { onoff: "1", shape: "3", mode: "2" };
/* While false, the value keys read "" -- a served channel with nothing to
   say, which the controller correctly refuses to cache. That is exactly the
   state a page is in before its reads land, and it is reachable without
   faking the rotation. */
let served = false;
const ctl = createController({
  getParam: (k) => {
    const b = String(k).replace(/^[^:]+:/, "");
    if (b === "ui_hierarchy") return JSON.stringify(HIER);
    if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
    if (!served && b in store) return "";
    return b in store ? store[b] : "";
  },
  setParam: (k, v) => { store[String(k).replace(/^[^:]+:/, "")] = String(v); },
  announce: () => {},
  now: () => clock,
});
ctl.setLayout(LAYOUT_MOVY);
ctl.load({ prefix: "synth" });

const shot = () => {
  const fb = createFramebuffer();
  ctl.render(drawContext(fb));
  return Buffer.from(fb.pixels).toString("base64");
};
/* THE DEVICE DRAWS EVERY TICK, AND THAT IS THE WHOLE MECHANISM.
   The store only learns a value when the RENDERER observes one, so a probe
   that ticks without drawing never shows the widgets the placeholder and
   passes with the bug fully present. Ticking and drawing together is what the
   grid actually does while its reads trickle in. */
const step = () => { ctl.tick(); shot(); };
const landed = () => ["onoff", "shape", "mode"]
  .every((k) => ctl.state.values[k] !== undefined);

/* Settle the page with the values still unserved. */
for (let i = 0; i < 20; i++) step();
ok(!landed(), "the fixture really does start with no values -- otherwise the " +
   "arrival below is not an arrival and this whole test is vacuous");

/* Now the module answers. Ticks do NOT advance the clock, so the frame taken
   the moment the last value lands sits at t=0 of any animation the arrival
   started: maximally mid-flight, which is the easiest possible case to catch. */
served = true;
let guard = 0;
while (!landed() && guard++ < 200) step();
ok(landed(), "the values landed");

const onArrival = shot();
clock += 900;
const afterArrival = shot();

ok(onArrival === afterArrival,
   "the frame at the instant the values arrive is ALREADY the settled one -- " +
   "an arrival is not a change, and a page must not animate itself in");

/* THE OTHER HALF. Deleting the animations passes everything above.

   Driven on the WAVEFORM, not the switch: #323 cut the switch fill, so a flip
   is settled on every frame and would report "no animation" whether or not the
   arrival fix is present -- which is a positive control that cannot fail. */
const slotOf = (k) => (ctl.page.keys || []).indexOf(k);
const sw = slotOf("shape");
ok(sw >= 0, "the waveform reached the page");

const before = shot();
/* Turn it DOWN: the fixture rests at Square (index 3 of 5), so down is a real
   step either way. An enum is gated at 4 raw detents per option. */
for (let i = 0; i < 6; i++) { clock += 20; ctl.onKnobTurn(sw, -1, clock); }
clock += 40;
const mid = shot();
clock += 900;
const settled = shot();

ok(mid !== before, "the turn changed the picture at all");
ok(mid !== settled,
   "a real CHANGE still animates -- without this, suppressing the arrival by " +
   "deleting the animation passes every assertion above");

process.exit(fail ? 1 : 0);
'
