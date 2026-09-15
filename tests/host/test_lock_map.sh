#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE LOCK MAP: which of the sixteen steps carry a lock.
#
# A lock is invisible until you hold its step, so finding one made earlier
# meant holding all sixteen and watching for an inverted band -- on a page that
# might not be the page it lives on. The audit called this the gap that
# dominates the feature: without it locking is a write-only medium, so you stop
# making locks you might want to revise.
#
# It is a PEEK and not a strip because the screen has no room for a strip: the
# header is the held-knob readout and the grid is eight cells. So it must
# appear in exactly one state and cost nothing in every other.

fail() { echo "FAIL: $1"; exit 1; }
command -v node >/dev/null || { echo "SKIP: node not available"; exit 0; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/t.mjs" <<EOF
const REPO = "$PWD";
EOF
cat >> "$tmp/t.mjs" <<'EOF'
import fs from "fs";
const { createController, LAYOUT_MOVY } = await import(REPO + "/src/shared/param_pages/page_controller.mjs");
const { createFramebuffer, drawContext } = await import(REPO + "/tools/param-pages/harness.mjs");
const mods = JSON.parse(fs.readFileSync(REPO + "/tests/fixtures/module-contracts.json", "utf8")).modules;
const m = mods.find((x) => x.id === "9w9");
if (!m) { console.log("SKIP: no 9w9 contract"); process.exit(0); }
const str = (v) => (typeof v === "string" ? v : JSON.stringify(v));

let held = -1, masks = "34866 2082";   /* page: 1,5,11   union: those + 3,14 */
const reads = [];
function getParam(key) {
    if (key === "synth:ui_hierarchy") return str(m.ui_hierarchy);
    if (key === "synth:chain_params") return str(m.chain_params);
    if (key === "lanes:step_locks") { reads.push(key); return masks; }
    if (key.endsWith(":held")) return "";
    if (key.endsWith(":modulated")) return "0";
    return "74";
}
let clock = 1000;
const ctrl = createController({ getParam, setParam: () => true, announce: () => {},
                                heldStep: () => held, now: () => clock });
ctrl.load({ slot: 0, component: "synth", prefix: "synth" });
ctrl.setLayout(LAYOUT_MOVY);
ctrl.dismissHint && ctrl.dismissHint();
ctrl.goToPage(ctrl.pages.findIndex((p) => (p.keys || []).includes("bd_c_tune")), { remember: false });
/* The band the panel rises into: the rule and the footer, rows 55..63. */
const ink = ({ settle = true } = {}) => {
    for (let i = 0; i < 14; i++) ctrl.tick();
    /* The panel SLIDES, so the FIRST frame of a gesture is still off the
     * bottom edge -- a probe that renders once and measures it is measuring
     * the animation's starting position, not the feature. Settle means: draw
     * a frame, let time pass, draw the one we score. */
    if (settle) {
        ctrl.renderOverlays(drawContext(createFramebuffer()), { clearScreen: () => {} });
        clock += 200;
    }
    const fb = createFramebuffer();
    ctrl.render(drawContext(fb), { title: "9W9" });
    ctrl.renderOverlays(drawContext(fb), { clearScreen: () => {} });
    let n = 0;
    for (let y = 55; y <= 63; y++) n += fb.pixels.slice(y*128, y*128+128).reduce((a,p)=>a+p, 0);
    return n;
};
const bad = [];
held = -1; const idle = ink();
held = 5;  const mapped = ink();
if (!(mapped > idle + 40)) bad.push("no map appeared while a step was held (" + idle + " -> " + mapped + ")");

/* AND IT STAYS UP WHILE A LOCK IS BEING SET. Holding a step and turning a
 * knob IS the p-lock gesture, so dismissing on touch -- which is what this
 * first did -- hid the strip during the one action it exists to support. */
ctrl.onKnobTouch(2, true);
const touchedInk = ink();
if (!(touchedInk > idle + 40))
    bad.push("the map left when a knob was touched -- that is the p-lock gesture (" + touchedInk + ")");
ctrl.onKnobTurn(2, 1, Date.now());
const turnedInk = ink();
if (!(turnedInk > idle + 40))
    bad.push("the map left when the lock was actually written (" + turnedInk + ")");
ctrl.onKnobTouch(2, false);

/* IT RISES, and the frame it rises through is the evidence. A panel that is
 * simply present on the first frame and absent on the last is what this
 * replaced -- appearing in place over the footer reads as corruption. */
held = -1; ink();
held = 3;
const first = ink({ settle: false });           /* still off the bottom edge */
clock += 45; const mid = ink({ settle: false });
clock += 200; const done = ink({ settle: false });
if (!(first < mid && mid < done))
    bad.push("the map did not slide in (" + first + " -> " + mid + " -> " + done + ")");

/* ...and it slides back OUT, which means it must outlive the read: the step is
 * released, so lanes:step_locks answers nothing, and the last map has to be
 * kept for the way down. */
held = -1;
const leaving = ink({ settle: false });
if (!(leaving > idle + 40)) bad.push("the map vanished instead of sliding out (" + leaving + ")");
clock += 300;
const gone = ink({ settle: false });
if (gone > idle + 40) bad.push("the map never left (" + gone + ")");

/* THE HELD STEP'S OUTLINE FLASHES, and the LOCK MARKS DO NOT.
 * A static frame is one more thing on a strip of sixteen small marks and the
 * eye does not find it; the cell that is CHANGING is the one the eye goes to.
 * The marks must hold still, or the blink competes with the two shapes that
 * mean "locked". */
held = 8; ink();
const cellInk = (fb, i) => {
    let n = 0;
    for (let y = 55; y <= 63; y++)
        for (let x = i * 8; x < i * 8 + 8; x++) n += fb.pixels[y * 128 + x] ? 1 : 0;
    return n;
};
{
    const seenHeld = new Set(), seenMark = new Set();
    for (let i = 0; i < 14; i++) {
        clock += 90;
        const fb = createFramebuffer();
        ctrl.render(drawContext(fb), { title: "9W9" });
        ctrl.renderOverlays(drawContext(fb), { clearScreen: () => {} });
        seenHeld.add(cellInk(fb, 8));
        seenMark.add(cellInk(fb, 1));      /* a locked step that is NOT held */
    }
    if (seenHeld.size < 2) bad.push("the held step's outline never changed -- it does not flash");
    if (seenMark.size !== 1) bad.push("a lock MARK flickered; only the outline may blink");
}
held = -1; ink();

/* ONE READ PER GESTURE, not per frame: the panel is drawn every tick and the
 * masks are fetched when the held step changes. */
held = -1; ink(); reads.length = 0;
held = 7; ink(); ink(); ink();
if (reads.length !== 1) bad.push("expected ONE step_locks read per gesture, got " + reads.length);

/* A READ THAT DID NOT COMPLETE IS NOT AN EMPTY CLIP. Drawing a blank map for
 * a failed read is the most misleading answer this panel can give: it says
 * "you have no locks" when it means "I could not ask". */
held = -1; ink();
masks = null; held = 9;
const failed = ink();
if (failed > idle + 40) bad.push("a failed read drew a map anyway (" + failed + ")");
masks = "34866 2082";

/* A lock made DURING a hold must be visible on the next hold of that same
 * step: the cache is per gesture, not per step number. Hardware found this --
 * every step but the one you had just edited was right. */
held = -1; ink();
masks = "0 0";        /* nothing locked anywhere */
held = 6; const before = ink();
held = -1; ink();
masks = "64 64";      /* ...and now step 6 carries one */
held = 6; const after = ink();
if (!(after > before + 10))
    bad.push("holding the same step twice re-used the first read (" + before + " -> " + after + ")");
masks = "34866 2082";

if (bad.length) { for (const b of bad) console.log("FAIL: " + b); process.exit(1); }
console.log("PASS: lock map (appears on the gesture, one read, STAYS through the lock, silent on a failed read)");
EOF
node "$tmp/t.mjs" || fail "the lock map does not behave"

# The masks are the SHIM's, computed by walking steps through the same
# step->phase function the write and the :held read use -- not by inverting the
# grid arithmetic, which would be a second implementation of the one thing this
# feature keeps being bitten by duplicating.
body=$(awk '/^static int shadow_lanes_step_locks/,/^}/' src/host/shadow_chain_mgmt.c)
[ -n "$body" ] || fail "shadow_lanes_step_locks is gone"
echo "$body" | grep -q 'shadow_lanes_step_phase(slot, i' \
  || fail "the map must walk STEPS through the shared step->phase function"
echo "$body" | grep -q 'if (len < 0) return -1' \
  || fail "a failed phases read must answer -1, not an empty map"
grep -q 'strcmp(sub, "phases")' src/modules/chain/dsp/chain_lanes.c \
  || fail "the chain must serve lanes:phases"
