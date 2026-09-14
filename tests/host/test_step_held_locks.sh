#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# HOLD A STEP AND SEE WHAT IS LOCKED ON IT -- the READ half of the p-lock
# gesture, which was missing while the write half worked. You could set values
# on a step and never see one again, which is why a working feature was
# reported as broken.
#
# The model is Elektron's, and each clause below is one of its rules:
#   - holding a trig shows what THAT step will play; unlocked params keep
#     showing the track's value,
#   - an encoder turn continues from the value on screen (so a locked param
#     edits its lock, an unlocked one creates one from the track value),
#   - releasing returns the display to the track's values.
#
# Driven through the real controller with the io hooks the device supplies, so
# what this exercises is the wiring, not a re-implementation of it.

fail() { echo "FAIL: $1"; exit 1; }
command -v node >/dev/null || { echo "SKIP: node not available"; exit 0; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/t.mjs" <<EOF
const REPO = "$PWD";
EOF
cat >> "$tmp/t.mjs" <<'EOF'
const { createController, LAYOUT_MOVY } = await import(REPO + "/src/shared/param_pages/page_controller.mjs");

/* 9W9's own captured contract: the module the gesture was reported broken on,
 * and a real hierarchy rather than a hand-made one that plans differently. */
const fs = await import("fs");
const mods = JSON.parse(fs.readFileSync(REPO + "/tests/fixtures/module-contracts.json", "utf8")).modules;
const m = mods.find((x) => x.id === "9w9");
if (!m) { console.log("SKIP: no 9w9 contract in the fixture"); process.exit(0); }
const str = (v) => (typeof v === "string" ? v : JSON.stringify(v));
const HIER = str(m.ui_hierarchy);
const PARAMS = str(m.chain_params);
const LOCKED = "bd_c_tune";      /* locked on step 5 */
const FREE = "bd_c_decay";       /* on the same page, locked nowhere */

let held = -1;                       /* the step under the finger */
const LOCK = { 5: { [LOCKED]: "20 1" } };  /* step 5 locks it at 20 */
const writes = [];
function getParam(key) {
    if (key === "synth:ui_hierarchy") return HIER;
    if (key === "synth:chain_params") return PARAMS;
    if (key.endsWith(":held")) {
        if (held < 0) return "";
        const k = key.slice("synth:".length, -":held".length);
        return (LOCK[held] && LOCK[held][k]) || "";
    }
    if (key.endsWith(":modulated")) return "0";
    return "100";                                  /* the BASE, what the track does */
}
const ctrl = createController({
    getParam, setParam: (k, v) => writes.push(k + "=" + v),
    announce: () => {}, heldStep: () => held,
});
ctrl.load({ slot: 0, component: "synth", prefix: "synth" });
ctrl.setLayout(LAYOUT_MOVY);
ctrl.dismissHint && ctrl.dismissHint();
const ticks = (n) => { for (let i = 0; i < n; i++) ctrl.tick(); };
const decOf = (k) => {
    const p = ctrl.page;
    const i = p.keys.indexOf(k);
    const d = ctrl.decorations;
    return d ? d[i] : null;
};
const bad = [];

ticks(12);                                   /* values land */
if (decOf(LOCKED)) bad.push("a lock was shown with no step held");

held = 5; ticks(12);
const dc = decOf(LOCKED);
if (!dc || !dc.locked) bad.push("holding a step did not show the lock on cutoff");
else if (String(dc.value) !== "20") bad.push("the lock showed " + dc.value + ", not the locked 20");
if (dc && !dc.exact) bad.push("a point sitting ON the step must report exact");
if (decOf(FREE)) bad.push("an unlocked param must keep showing the track value, not a lock");

/* A turn continues from the LOCK (20), not from the base (100). */
writes.length = 0;
ctrl.onKnobTurn(ctrl.page.keys.indexOf(LOCKED), 1, 1000);
ticks(6);
/* The write NAMES THE STEP rather than being a plain component write the shim
 * has to attribute from whatever is held when it arrives. That race is the
 * leak: the shim clears held_step on the frame carrying the note-off while the
 * UI reacts to the detent a tick later, so a write that does not name the step
 * lands on the TRACK. */
const w = writes.find((x) => x.startsWith("lanes:plock_step="));
if (!w) bad.push("a turn under a held step did not write lanes:plock_step: " + JSON.stringify(writes));
else {
    const parts = w.split("=")[1].split(" ");
    if (parts[0] !== "synth" || parts[1] !== LOCKED)
        bad.push("the p-lock named the wrong parameter: " + w);
    if (Number(parts[2]) !== 5) bad.push("the p-lock named step " + parts[2] + ", not the held one (5)");
    const v = Number(parts[3]);
    if (!(v > 19 && v < 40)) bad.push("the turn continued from " + v + " -- it must walk from the LOCK (20), not the base (100)");
}
if (writes.some((x) => x.startsWith("synth:" + LOCKED + "=")))
    bad.push("a turn under a held step ALSO wrote the track value -- that is the leak");

/* Release: the lock display goes, and the knob walks from the base again. */
held = -1; ticks(12);
if (decOf(LOCKED)) bad.push("releasing the step left the lock on screen");
writes.length = 0;
ctrl.onKnobTurn(ctrl.page.keys.indexOf(LOCKED), 1, 2000);
ticks(6);
const w2 = writes.find((x) => x.startsWith("synth:" + LOCKED + "="));
if (!w2) bad.push("a turn after release wrote nothing");
else {
    const v2 = Number(w2.split("=")[1]);
    if (!(v2 > 99)) bad.push("after release the knob walked from " + v2 + " -- it must be back on the base (100)");
}

/* A "no" of any kind is never a value: an empty answer must not decorate. */
held = 9; ticks(12);                          /* step 9 locks nothing */
if (decOf(LOCKED) || decOf(FREE)) bad.push("an empty :held answer became a lock -- every 'no' must show nothing");

/*
 * AND IT MUST LOOK LIKE ELEKTRON'S, which is a PIXEL fact.
 *
 * Every Elektron manual that documents parameter locks says the same sentence:
 * "the graphics become inverted for the locked parameter, and the locked
 * parameter value is displayed". Ours drew the value into the knob and marked
 * the cell with a 2x2 corner, which is something you have to be told about.
 * A decoration object being correct says nothing about that, so this counts
 * INK in the locked cell's label band: an inverted band is a filled strip and
 * a label is a few strokes, so the difference is not subtle.
 */
const { createFramebuffer, drawContext } = await import(REPO + "/tools/param-pages/harness.mjs");
/*
 * The assertion is a SHAPE, not an ink count. An inverted band is a
 * CONTIGUOUS filled run -- the strip Elektron describes -- while a label is
 * strokes with gaps, so the longest horizontal run separates them cleanly at
 * any value width. Counting ink does not: "20" inverted has barely more lit
 * pixels than the word "Tune" drawn normally (43 -> 52 measured), so a
 * threshold on ink would either miss the inversion or fire on a long label.
 *
 * The band rows are measured, not assumed: diffing a held frame against an
 * unheld one puts row 0's label band at y 23..31 and row 1's at y 52..60.
 */
function bandRun(fb, slot) {
    const row = slot < 4 ? 0 : 1, c = slot % 4;
    const y0 = row === 0 ? 23 : 52, y1 = row === 0 ? 31 : 60;
    let best = 0;
    for (let y = y0; y <= y1; y++) {
        let run = 0;
        for (let x = c * 32; x < c * 32 + 32; x++) {
            run = fb.pixels[y * fb.width + x] ? run + 1 : 0;
            if (run > best) best = run;
        }
    }
    return best;
}
function shot() {
    const fb = createFramebuffer();
    ctrl.render(drawContext(fb), { title: "9W9" });
    return fb;
}
const lockedSlot = ctrl.page.keys.indexOf(LOCKED);
const freeSlot = ctrl.page.keys.indexOf(FREE);
held = -1; ticks(14);
const idle = shot();
held = 5; ticks(14);
const hold = shot();
if (!(bandRun(hold, lockedSlot) >= 8))
    bad.push("the locked cell's label band is not a filled STRIP while the step is held (longest run " +
             bandRun(hold, lockedSlot) + "px) -- Elektron's headline mark is the inversion, not a corner pixel");
if (bandRun(idle, lockedSlot) >= 8)
    bad.push("the band reads as inverted with NO step held (run " + bandRun(idle, lockedSlot) + "px)");
if (bandRun(hold, freeSlot) !== bandRun(idle, freeSlot))
    bad.push("an UNLOCKED cell changed under a held step -- it must keep showing the track's own value");

if (bad.length) { for (const b of bad) console.log("FAIL: " + b); process.exit(1); }
console.log("PASS: step-held locks (show, turn-from-lock, release, empty-is-not-a-value)");
EOF

node "$tmp/t.mjs" || fail "the step-held read half does not behave"

# The shim owns WHICH step, and both halves must read the same answer, or the
# value shown and the value a turn replaces can disagree.
grep -q 'shadow_get_held_step' src/shared/param_pages/page_controller.mjs \
  || fail "the controller must ask the DEVICE which step is held -- an io-only hook is invisible to every module-drawn grid"
grep -q 'shim_plock_held_step' src/schwung_shim.c \
  || fail "the shim must be the one deciding the held step"
body=$(awk '/THE READ HALF OF THE P-LOCK GESTURE/,/^}/' src/host/shadow_chain_mgmt.c)
echo "$body" | grep -q 'shadow_lanes_step_phase' \
  || fail "the :held read must use the SAME step->phase function as the write, or the value shown is not the value a turn replaces"

# A TRIGGER IS NOT LOCKED BY DEFAULT.
#
# Under a held step a write becomes a p-lock, so a momentary would re-fire at
# that step on every pass. Measured on `palette`, whose Main page carries
# `rnd_macro` on a knob: one fire per loop, and twenty seconds later the patch
# had walked through four unrelated sounds, with no undo and no base value to
# return to. Right for a retrig, a trap as the default every write-only param
# gets for free -- and the gesture that arms it is a brush of a knob,
# indistinguishable from the one that fires it.
ctl=src/shared/param_pages/page_controller.mjs
# BOTH entry points: a trigger can be fired by a turn and by a click, and
# guarding one leaves the same trap one gesture away.
n=$(grep -c 'TRIGGERS CANNOT BE LOCKED' "$ctl")
[ "$n" = "2" ] || fail "expected the refusal at BOTH trigger entry points (turn and click), found $n"
python3 - <<'PY' || fail "a refusal comes AFTER its fireTrigger -- it would fire and lock before refusing"
import sys
src = open("src/shared/param_pages/page_controller.mjs").read()
ok = True
i = 0
while True:
    i = src.find("if (meta.writeOnly)", i)
    if i < 0: break
    seg = src[i:i+2500]
    r = seg.find("TRIGGERS CANNOT BE LOCKED")
    f = seg.find("fireTrigger(")
    if r < 0 or f < 0 or r > f: ok = False
    i += 1
sys.exit(0 if ok else 1)
PY

# A LOCK IS DRAWN THE WAY MODULATION IS, and the point is that ONE picture
# cannot mean two things depending on which mode you are in.
#
# The lock used to replace the POINTER, so while you held a step the pointer
# was the step's value, and while the lane played the same lock back the
# pointer was the base with a mark at the driven value. Same cell, two
# grammars. Now the knob is pixel-identical between "an LFO is driving this to
# X" and "this step plays X"; what a held step adds is the corner mark and the
# inverted band, which say WHICH STEP rather than what value.
node - <<'JS' || fail "a held step and a modulated value no longer draw the same knob"
const REPO = process.cwd();
const { renderPageMovy } = await import(REPO + "/src/shared/param_pages/render_page_movy.mjs");
const { buildMetaIndex } = await import(REPO + "/src/shared/param_pages/param_meta.mjs");
const { createFramebuffer, drawContext } = await import(REPO + "/tools/param-pages/harness.mjs");
const metaIndex = buildMetaIndex([{ key: "cutoff", name: "Cutoff", type: "float", min: 0, max: 1, step: 0.01, default: 0.5 }]);
const page = { kind: "knobs", name: "P", keys: ["cutoff"] };
const knob = (values, decorations, modValues) => {
  const fb = createFramebuffer();
  renderPageMovy(drawContext(fb), { page, metaIndex, values, decorations, modValues,
    title: "T", pageIndex: 0, pageCount: 1, touched: -1, viz: [] });
  /* The knob's INTERIOR: columns 4..31 of cell 0, rows 10..22. The corner
   * mark lives at x=1..2 and the label band below row 23, and those two are
   * exactly what a held step is ALLOWED to add -- including them would make
   * this assertion trivially false and hide the thing it is checking. */
  let out = "";
  for (let y = 10; y <= 22; y++) out += fb.pixels.slice(y * 128 + 4, y * 128 + 32).join("");
  return out;
};
const base = { cutoff: "0.9" };
const modded = knob(base, null, { cutoff: "0.1" });
const locked = knob(base, [{ locked: true, value: "0.1" }], null);
const plain  = knob(base, null, null);
if (modded !== locked) { console.log("FAIL: locked knob differs from modulated knob"); process.exit(1); }
if (plain === locked)  { console.log("FAIL: a lock changed nothing in the knob"); process.exit(1); }
JS
