#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A REFUSAL BELONGS TO THE GESTURE THAT CAUSED IT.
#
# The check used to read `lanes:plock_reason` and `lanes:plock_refused` on the
# same tick as the write. The param channel is a request/response queue, not a
# function call, so the read can be served BEFORE the write it asks about --
# and what came back was the PREVIOUS gesture's outcome. One real refusal was
# then re-reported on every later gesture, forever.
#
# Reported from the device as "hank's tone knob says can't automate, that's
# wrong", on a parameter that locks perfectly well through the same path, and
# as the display being "inconsistent" -- which is what a race looks like from
# outside. The toast is the feature working as designed and lying anyway,
# which is worse than no toast: it teaches the user to distrust a feature that
# is not broken.

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
const str = (v) => (typeof v === "string" ? v : JSON.stringify(v));

let held = -1, seq = 7, reason = "0 ok", refused = "0 ok";
const notices = [], reads = [];
function getParam(key) {
    if (key === "synth:ui_hierarchy") return str(m.ui_hierarchy);
    if (key === "synth:chain_params") return str(m.chain_params);
    if (key === "lanes:plock_reason")  { reads.push(key); return reason; }
    if (key === "lanes:plock_refused") { reads.push(key); return refused; }
    if (key.endsWith(":held")) return "";
    if (key.endsWith(":modulated")) return "0";
    return "74";
}
const ctrl = createController({
    getParam, setParam: () => true,
    announce: (t) => notices.push(String(t)),
    heldStep: () => held,
    plockSeq: () => seq,
});
ctrl.load({ slot: 0, component: "synth", prefix: "synth" });
ctrl.setLayout(LAYOUT_MOVY);
ctrl.dismissHint && ctrl.dismissHint();
ctrl.goToPage(ctrl.pages.findIndex((p) => (p.keys || []).includes("bd_c_tune")), { remember: false });
for (let i = 0; i < 12; i++) ctrl.tick();

const bad = [];
const turn = () => { ctrl.onKnobTouch(0, true); ctrl.onKnobTurn(0, 1, Date.now()); ctrl.onKnobTouch(0, false); };
/* Long enough for BOTH waits -- the counter's and the longer one a
 * consumer without a counter has to take. */
const settle = () => { for (let i = 0; i < 12; i++) ctrl.tick(); };

/* 1. A LOCK THAT LANDED SAYS NOTHING -- and asks nothing. The counter moving
 *    is proof, so the registers are not even read. */
held = 4; notices.length = 0; reads.length = 0;
turn(); seq++; settle();
if (notices.some((t) => /not locked/i.test(t)))
    bad.push("a lock that landed still announced a refusal: " + JSON.stringify(notices));
if (reads.length)
    bad.push("a lock that landed still spent " + reads.length + " reads on the refusal registers");

/* 2. A STALE REGISTER CANNOT SPEAK. The counter moved, so this gesture took --
 *    whatever a leftover code from an earlier one says. This is the reported
 *    bug: the parameter locks and the screen calls it broken. */
reason = "3 unknown_param"; refused = "3 unknown_param";
held = 5; notices.length = 0;
turn(); seq++; settle();
if (notices.some((t) => /not locked/i.test(t)))
    bad.push("a stale refusal was reported over a lock that landed: " + JSON.stringify(notices));

/* 3. ...AND A REAL REFUSAL STILL SPEAKS. The counter did NOT move, so the
 *    registers are asked and believed. Silencing this is not the fix: the
 *    fallback is invisible -- the value is written to the whole track. */
held = 6; notices.length = 0;
turn(); /* seq unchanged */ settle();
if (!notices.some((t) => /not locked/i.test(t)))
    bad.push("a real refusal was swallowed: " + JSON.stringify(notices));

/* 4. A READ THAT DID NOT COMPLETE IS NOT A REFUSAL. */
reason = null; refused = null;
held = 7; notices.length = 0;
turn(); settle();
if (notices.some((t) => /not locked/i.test(t)))
    bad.push("a failed read became a refusal: " + JSON.stringify(notices));

if (bad.length) { for (const b of bad) console.log("FAIL: " + b); process.exit(1); }
console.log("ok  a refusal belongs to the gesture that caused it");
EOF
node "$tmp/t.mjs" || fail "refusal provenance"

# The judgement must NOT happen inline with the write: that is the race.
body=$(awk '/function writeUserValue/,/^    }/' src/shared/param_pages/page_controller.mjs)
echo "$body" | grep -q 'lanes:plock_reason' \
  && fail "the refusal registers are read inline with the write again -- that is the race"
grep -q 'judgePendingRefusal();' src/shared/param_pages/page_controller.mjs \
  || fail "nothing judges the recorded refusal"
# A consumer with no counter has only the delay, so the delay must outlast the
# param channel's own 100 ms deadline -- otherwise a write still in flight is
# judged by the previous gesture's registers, which is this very bug returning
# for module-drawn grids (9W9) alone.
node -e '
const fs = require("fs");
const src = fs.readFileSync("src/shared/param_pages/page_controller.mjs", "utf8");
const m = src.match(/REFUSAL_JUDGE_TICKS_NO_SEQ = (\d+)/);
if (!m) { console.log("FAIL: no counter-less wait"); process.exit(1); }
/* ~23 ms a tick; the deadline is 100 ms. */
if (Number(m[1]) * 23 <= 100) {
    console.log("FAIL: the counter-less wait (" + m[1] + " ticks) is inside the param deadline");
    process.exit(1);
}
console.log("ok  a consumer with no counter waits past the param deadline");
' || fail "the counter-less wait is too short"
echo "PASS: p-lock refusals carry provenance"
