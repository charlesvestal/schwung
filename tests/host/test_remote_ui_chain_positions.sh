#!/bin/bash
#
# The Remote UI page sees every chain position the device reports, offers the
# next empty one, and never resets a component on a read that did not complete.
#
# It knew fx1/fx2/midi_fx1 only, so a third audio FX loaded, ran and made sound
# while the browser could not see it. And slot_info turned a timed-out read
# into "", which the page took as "this position is empty" and answered by
# wiping the component's cached hierarchy and values.
#
# Runs the page's own functions (extracted, as test_remote_ui_panel_visible.sh
# does), not a grep of them.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JS="$ROOT/schwung-manager/static/remote-ui.js"
[ -f "$JS" ] || { echo "FAIL: $JS not found"; exit 1; }

node - "$JS" <<'NODE'
const fs = require("fs");
const src = fs.readFileSync(process.argv[2], "utf8");

function extract(sig) {
    const re = new RegExp("function " + sig + "\\s*\\{[\\s\\S]*?\\n    \\}");
    const m = src.match(re);
    if (!m) { console.error("FAIL: function " + sig + " not found"); process.exit(1); }
    return new Function("return (" + m[0] + ")")();
}
function check(cond, msg) { if (!cond) { console.error("FAIL: " + msg); process.exit(1); } }

const slotInfoIncoming = extract("slotInfoIncoming\\(msg, keys\\)");
const slotLoaderPositions = extract("slotLoaderPositions\\(s\\)");
const componentRank = extract("componentRank\\(k\\)");

const keys = ["synth"];
for (let i = 1; i <= 8; i++) keys.push("fx" + i, "midi_fx" + i);

// --- every listed position, including past the old fixed two -------------
let inc = slotInfoIncoming({ synth: "obxd", fx: ["freeverb", "", "tape-echo2"], midi_fx: [] }, keys);
check(inc.fx3 === "tape-echo2", "fx3 must be seen (got " + JSON.stringify(inc.fx3) + ")");
check(inc.fx2 === "", "a hole in front of a module is an empty position");
check(inc.fx4 === "", "a position past the listing is empty, so a removed FX is dropped");
check(inc.synth === "obxd", "synth");

// --- null vs "": an unread section is LEFT OUT, never reported empty -------
inc = slotInfoIncoming({ synth: "obxd", fx: null, midi_fx: ["arp"], unknown: ["fx"] }, keys);
for (let i = 1; i <= 8; i++) {
    check(!("fx" + i in inc), "fx" + i + " must be absent (kept) when the fx section was not read");
}
check(inc.midi_fx1 === "arp", "a read section still applies beside an unread one");
inc = slotInfoIncoming({ synth: "", fx: [], midi_fx: [], unknown: ["synth"] }, keys);
check(!("synth" in inc), "an unread synth must be kept, not emptied");
check(inc.fx1 === "", "a known-empty fx section empties fx1");

// --- the loader: listed positions plus ONE '+' while there is room ---------
const comps = (s) => slotLoaderPositions(s).map((p) => p.comp).join(",");
check(comps({ fxCount: 0, midiFxCount: 0, fxCap: 8, midiFxCap: 8 }) === "midi_fx1,synth,fx1",
      "empty chain: one '+' per section");
check(comps({ fxCount: 3, midiFxCount: 1, fxCap: 8, midiFxCap: 8 }) ===
      "midi_fx1,midi_fx2,synth,fx1,fx2,fx3,fx4",
      "three FX: their boxes, then fx4 as the '+'");
check(comps({ fxCount: 8, midiFxCount: 0, fxCap: 8, midiFxCap: 8 }).split(",").filter((c) => /^fx/.test(c)).length === 8,
      "a full chain offers no ninth box");

// --- signal-flow order for grown keys ---------------------------------------
const ordered = ["fx3", "synth", "midi_fx2", "fx1", "midi_fx1"].sort((a, b) => {
    const ra = componentRank(a), rb = componentRank(b);
    return (ra[0] - rb[0]) || (ra[1] - rb[1]);
});
check(ordered.join(",") === "midi_fx1,midi_fx2,synth,fx1,fx3", "signal-flow order: " + ordered.join(","));

console.log("  remote-ui chain positions: OK");
NODE

# No fixed position list may come back: positions come from the slot_info.
if grep -q 'SLOT_LOADER_POSITIONS' "$JS"; then
    echo "FAIL: a fixed loader position list reappeared — use slotLoaderPositions(s)"; exit 1
fi
if grep -qE 'msg\.fx[1-4] \|\|' "$JS"; then
    echo "FAIL: master/slot info read through the fixed fx1..fx4 fields again"; exit 1
fi

echo "PASS: the Remote UI sees every chain position and keeps what an unread section held"
