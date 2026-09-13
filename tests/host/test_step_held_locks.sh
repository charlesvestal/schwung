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
const w = writes.find((x) => x.startsWith("synth:" + LOCKED + "="));
if (!w) bad.push("a turn under a held step wrote nothing");
else {
    const v = Number(w.split("=")[1]);
    if (!(v > 19 && v < 40)) bad.push("the turn continued from " + v + " -- it must walk from the LOCK (20), not the base (100)");
}

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
