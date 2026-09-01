#!/usr/bin/env bash
# Snapshot recall planner: what a recall writes, and what it counts as skipped.
#
# The counts are the assertion that matters. A recall that silently restores
# half the rig is indistinguishable from one that worked, so every branch that
# declines to write must show up in `skipped` and be attributable in `reasons`.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { parseSlotSnapshot, parseMasterFxSnapshot, planRestore, recallMessage }
    from "./src/shared/snapshot.mjs";

let fails = 0;
function eq(what, got, want) {
    const g = JSON.stringify(got), w = JSON.stringify(want);
    if (g !== w) { console.error(`FAIL ${what}\n  got  ${g}\n  want ${w}`); fails++; }
}

/* ---- parsing ---------------------------------------------------------- */

const slotJson = JSON.stringify({
    name: "Lead", version: 1, modified: false,
    chain: {
        synth: { module: "braids", config: { state: { pitch: 3 } }, bypassed: 0 },
        midi_fx: [ { type: "arp", params: { state: "rate=4" }, bypassed: 1 } ],
        audio_fx: [
            { type: "cloudseed", params: { state: { mix: 0.4 } }, bypassed: 0 },
            { type: "denis",     params: {},                     bypassed: 0 }
        ]
    }
});
const recs = parseSlotSnapshot(slotJson);
eq("slot prefixes", recs.map(r => r.prefix),
   ["synth", "midi_fx1", "fx1", "fx2"]);
eq("object state is re-serialised", recs[0].state, JSON.stringify({ pitch: 3 }));
eq("string state passes through", recs[1].state, "rate=4");
eq("bypass carried", recs[1].bypassed, 1);
eq("no state key reads as null", recs[3].state, null);

eq("malformed slot file yields nothing", parseSlotSnapshot("{not json"), []);
eq("empty slot marker yields nothing", parseSlotSnapshot("{}"), []);

const mfx = parseMasterFxSnapshot(
    JSON.stringify({ id: "mverb", path: "/x.so", state: { size: 9 } }), 2);
eq("master fx prefix is 1-based", mfx.map(r => r.prefix), ["master_fx:fx3"]);
eq("master fx state", mfx[0].state, JSON.stringify({ size: 9 }));
eq("empty master position yields nothing", parseMasterFxSnapshot("{}", 0), []);

/* ---- planning --------------------------------------------------------- */

const live = {
    synth:    "braids",       /* unchanged     -> write   */
    midi_fx1: "chord",        /* swapped       -> skipped */
    fx1:      "cloudseed",    /* unchanged     -> write   */
    fx2:      "denis"         /* no state      -> skipped */
    /* master_fx:fx3 absent    -> removed      -> skipped */
};
const plan = planRestore(recs.concat(mfx), live);
eq("writes only the matched, state-bearing positions",
   plan.writes.map(w => w.prefix), ["synth", "fx1"]);
eq("skipped counts every position that did not come back", plan.skipped, 3);
eq("reasons are attributable",
   plan.reasons.map(r => [r.prefix, r.reason]),
   [["midi_fx1","swapped"], ["fx2","nostate"], ["master_fx:fx3","empty"]]);

/* An EMPTY position in the snapshot is not a miss — it had nothing to give
 * back. Counting it would make every partly-filled rig report skips forever. */
eq("empty-in-snapshot is not counted",
   planRestore([{ prefix: "fx3", moduleId: "", state: null, bypassed: 0 }], {}).skipped, 0);

/* A perfect recall reports no number at all. */
eq("clean recall message", recallMessage(0), ["Snapshot restored"]);
eq("lossy recall message", recallMessage(3), ["Snapshot restored", "3 skipped"]);

if (fails) { console.error(`\n${fails} assertion(s) failed`); process.exit(1); }
console.log("PASS test_snapshot_plan");
'
