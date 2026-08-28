#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE ANIMATIONS HAVE TO REACH THE RENDERER FROM THE CONTROLLER.
#
# Every animated widget guards on `anim && typeof nowMs === "number"`, so an
# undefined store draws the settled frame forever -- silently, and identically
# to a correct render of a value that is not moving. createAnimState was
# written, exported, unit-tested and never CALLED: the switch fill, the waveform
# morph and the enum square resize were all inert on hardware from the day they
# shipped, and every test passed because the renderer tests hand `anim` in
# DIRECTLY. They prove the renderer. Nothing proved the wiring.
#
# THE SWITCH IS NO LONGER AN ANIMATED WIDGET -- its fill was removed because it
# read as distracting on hardware, and it now toggles between two settled
# frames. So this drives the WAVEFORM morph instead. The page still declares a
# switch, and it is still asserted to be settled on every frame, which is the
# other half of the same wiring question: a widget that should not move must
# not start moving because a store arrived.
#
# This drives the real controller and asserts that a value change produces a
# DIFFERENT PICTURE part-way through than at rest. That cannot pass with the
# store missing, and it does not care which widget or which easing.
#
# The trigger flash had this exact bug once, was fixed, and a note was left at
# its call site. The animations that arrived later reproduced it one field away
# from that note -- so the defence has to be a test, not a comment.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the anim wiring test" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, LAYOUT_MOVY }
  from "./src/shared/param_pages/page_controller.mjs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* A waveform (5 shapes), which animates, and a switch (Off/On), which no
   longer does. Both are asserted: the morph must move, the switch must not. */
const CHAIN_PARAMS = [
  { key: "onoff", name: "Gate", type: "enum", options: ["Off", "On"] },
  { key: "shape", name: "Shape", type: "enum",
    options: ["Sine", "Tri", "Saw", "Square", "Noise"] },
];
const HIER = { modes: null, levels: { root: { label: "T",
  knobs: ["onoff", "shape"], params: CHAIN_PARAMS.map((p) => ({ key: p.key })) } } };

let clock = 1000;
const store = { onoff: "0", shape: "0" };
const ctl = createController({
  getParam: (k) => {
    const b = String(k).replace(/^[^:]+:/, "");
    if (b === "ui_hierarchy") return JSON.stringify(HIER);
    if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
    return b in store ? store[b] : "";
  },
  setParam: (k, v) => { store[String(k).replace(/^[^:]+:/, "")] = String(v); },
  announce: () => {},
  now: () => clock,
});
/* The MOVY grid, not the default LAYOUT_DIAL -- the animated widgets live in
   that renderer alone, so a test on the default layout exercises none of them
   and passes with the store missing. */
ctl.setLayout(LAYOUT_MOVY);
ctl.load({ prefix: "synth" });
for (let i = 0; i < 12; i++) ctl.tick();

const shot = () => {
  const fb = createFramebuffer();
  ctl.render(drawContext(fb));
  return { fb, key: Buffer.from(fb.pixels).toString("base64") };
};

/* The store must exist at all. Checked first so a missing one reports as
   itself rather than as "nothing animated". */
ok(!!ctl.state.anim, "the controller owns an animation store");

const slotOf = (k) => (ctl.page.keys || []).indexOf(k);
const wv = slotOf("shape");
const sw = slotOf("onoff");
ok(wv >= 0, "the waveform reached the page");
ok(sw >= 0, "the switch reached the page");

/* An enum is gated at 4 raw detents per option. */
const step = (slot) => { for (let i = 0; i < 6; i++) { clock += 20; ctl.onKnobTurn(slot, 1, clock); } };

const before = shot();
step(wv);

/* Mid-flight and settled. The waveform morph is 100ms, so 40ms in is inside it
   and 600ms is well past. */
clock += 40;
const mid = shot();
clock += 600;
const settled = shot();

ok(mid.key !== before.key, "the shape change changed the picture at all");
ok(mid.key !== settled.key,
   "the frame 40ms into the morph differs from the settled one -- with no store " +
   "wired through, every frame after a change is already the settled one");
/* And it really does come to rest, rather than animating forever. */
clock += 600;
ok(shot().key === settled.key, "and it settles");

/* THE SWITCH IS THE OTHER HALF: it must NOT move. Its fill was removed for
   reading as distracting on hardware, and the store is wired, so the only
   thing that can put motion back is the widget. Every frame after the flip is
   the settled one -- including the very first, which is where a re-introduced
   animation would show. */
const preFlip = shot();
step(sw);
const flipped = [];
for (const dt of [0, 40, 80, 160, 600]) { clock += dt; flipped.push(shot().key); }
ok(flipped[0] !== preFlip.key, "the flip changed the picture at all");
ok(flipped.every((k) => k === flipped[0]),
   "the switch is settled on every frame after the flip -- it toggles, it does " +
   "not animate");

process.exit(fail ? 1 : 0);
'
