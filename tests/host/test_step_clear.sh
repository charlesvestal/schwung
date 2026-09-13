#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# REMOVING ONE STEP'S AUTOMATION -- the grain that did not exist.
#
# `clear`, `clear_clip`, `clear_param` and `clear_target` each take a whole
# lane or more, so getting rid of a single bad p-lock meant throwing away that
# parameter's entire automation. Elektron removes a lock by PRESSING THE
# ENCODER of the parameter; Move has no encoder press, so the gesture is the
# one this grid already uses for instance copy/clear -- hold DELETE, then PICK
# with a knob touch, or release without picking to take the whole step.
#
# What this pins is the shape of that gesture, because every part of it is a
# decision that can silently invert: which button, what a pick means, what
# happens with no pick, and -- the dangerous one -- that Delete must not fall
# through to Move, which deletes the CLIP.

fail() { echo "FAIL: $1"; exit 1; }
command -v node >/dev/null || { echo "SKIP: node not available"; exit 0; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/t.mjs" <<EOF
const REPO = "$PWD";
EOF
cat >> "$tmp/t.mjs" <<'EOF'
import fs from "fs";
const { createController, LAYOUT_MOVY } = await import(REPO + "/src/shared/param_pages/page_controller.mjs");
const mods = JSON.parse(fs.readFileSync(REPO + "/tests/fixtures/module-contracts.json", "utf8")).modules;
const m = mods.find((x) => x.id === "9w9");
if (!m) { console.log("SKIP: no 9w9 contract"); process.exit(0); }
const str = (v) => (typeof v === "string" ? v : JSON.stringify(v));

let held = -1;
const LOCKS = { 5: { bd_c_tune: "20 1", bd_c_decay: "31 1" } };
const writes = [];
function getParam(key) {
    if (key === "synth:ui_hierarchy") return str(m.ui_hierarchy);
    if (key === "synth:chain_params") return str(m.chain_params);
    if (key.endsWith(":held")) {
        if (held < 0) return "";
        const k = key.slice("synth:".length, -":held".length);
        return (LOCKS[held] && LOCKS[held][k]) || "";
    }
    if (key.endsWith(":modulated")) return "0";
    return "74";
}
const ctrl = createController({
    getParam, setParam: (k, v) => { writes.push(k + "=" + v); return true; },
    announce: () => {}, heldStep: () => held,
});
ctrl.load({ slot: 0, component: "synth", prefix: "synth" });
ctrl.setLayout(LAYOUT_MOVY);
ctrl.dismissHint && ctrl.dismissHint();
const ticks = (n) => { for (let i = 0; i < n; i++) ctrl.tick(); };
const page = ctrl.pages.findIndex((p) => (p.keys || []).includes("bd_c_tune"));
ctrl.goToPage(page, { remember: false });
const slotOf = (k) => ctrl.page.keys.indexOf(k);
const bad = [];

held = 5; ticks(14);
const dec = () => ctrl.decorations || [];
if (!dec()[slotOf("bd_c_tune")]) bad.push("setup: the lock should be showing before we clear it");

/* PICK: Delete held, touch that knob -> only that parameter's lock goes. */
writes.length = 0;
if (ctrl.onEditCc(119, true) !== true) bad.push("Delete must be CLAIMED while a step is held -- unclaimed it reaches Move, which deletes the CLIP");
ctrl.onKnobTouch(slotOf("bd_c_tune"), true);
const w = writes.find((x) => x.startsWith("lanes:clear_step="));
if (!w) bad.push("a pick must write lanes:clear_step");
else if (!/synth bd_c_tune$/.test(w)) bad.push("a pick must name the parameter: " + w);
ctrl.onKnobTouch(slotOf("bd_c_tune"), false);
ticks(2);
if (dec()[slotOf("bd_c_tune")]) bad.push("the cleared parameter must stop showing a lock");
if (!dec()[slotOf("bd_c_decay")]) bad.push("the OTHER locked parameter on that step must be untouched");

/* Releasing AFTER a pick must not then clear the whole step. */
writes.length = 0;
ctrl.onEditCc(119, false);
if (writes.some((x) => x === "lanes:clear_step=")) bad.push("releasing after a pick must not also clear the whole step");

/* NO PICK: Delete pressed and released -> the whole step. */
held = -1; ticks(6); held = 5; ticks(14);
writes.length = 0;
ctrl.onEditCc(119, true);
ctrl.onEditCc(119, false);
if (!writes.some((x) => x === "lanes:clear_step=")) bad.push("releasing without a pick must clear the WHOLE step (empty argument)");

/* With NO step held, Delete is the instance gesture again, never this one. */
held = -1; ticks(6);
writes.length = 0;
ctrl.onEditCc(119, true);
if (writes.some((x) => x.startsWith("lanes:clear_step"))) bad.push("Delete with no step held must not clear automation");
ctrl.onEditCc(119, false);

if (bad.length) { for (const b of bad) console.log("FAIL: " + b); process.exit(1); }
console.log("PASS: step clear (pick one, release for all, claimed from Move, no step = no clear)");
EOF
node "$tmp/t.mjs" || fail "the step-clear gesture does not behave"

# The chain verb, and the two things about it that are not obvious.
lanes=src/modules/chain/dsp/chain_lanes.c
body=$(awk '/strcmp\(sub, "clear_point"\)/,/^    }$/' "$lanes")
[ -n "$body" ] || fail "chain must serve lanes:clear_point"
echo "$body" | grep -q 'LANE_MIN_POINT_BEATS' \
  || fail "\"on this step\" must be the same window lane_write replaces in, or a clear and a p-lock disagree about which point is which"
echo "$body" | grep -q 'lane_release_one' \
  || fail "a lane emptied by a clear must RELEASE its override, or the parameter stays stuck at the value it last drove"
echo "$body" | grep -q 'lane_undo_take' \
  || fail "the clear must be undoable, like every other clear verb"

# ...and the host must refuse rather than forward a phase-less clear: the chain
# would read a missing phase as 0 and take the downbeat's automation.
grep -q 'lanes:clear_step' src/host/shadow_chain_mgmt.c \
  || fail "the host must translate lanes:clear_step -- the chain knows only phases"
