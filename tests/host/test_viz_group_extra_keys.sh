#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A VIZ GROUP MUST BE ABLE TO CARRY extra_keys.
#
# resolveViz builds a declared GROUP from `viz: {group, role, kind}` on several
# keys, then reads that group's extra_keys back off its members:
#
#     for (const r of Object.values(g.roles)) {
#         const ek = declaredExtraKeys(r.viz);
#
# The roles were stored as `{ key, slot, span }` with no `viz` field, so that
# lookup returned null for every member and a group could NEVER carry
# extra_keys -- while the comment directly above it said it could. The failure
# is invisible in the contract and silent at runtime: a spanning widget whose
# values live on another page simply draws its "no answer" state forever, which
# reads as a broken widget rather than as a missing value.
#
# (Found by Hinge, whose output-waveform widget spans the phase cells on one
# page and needs the OP2 ratio/level/feedback values from another.)
#
# SINGLES already worked -- that branch passes extraKeys explicitly -- so this
# pins the GROUP path specifically, and asserts the singles path stays working
# so a fix cannot trade one for the other.

node --input-type=module -e '
import { resolveViz } from "./src/shared/param_pages/viz.mjs";
import { registerOverlayWidgets } from "./src/shared/param_pages/widget_registry.mjs";

registerOverlayWidgets({ widgetKinds: ["custom:t"], drawCell() {} }, "test");

const mk = (metas) => ({ getOrGuess: (k) => metas[k] || { key: k }, byKey: metas });
let fail = 0;
const check = (label, got, want) => {
    const g = JSON.stringify(got), w = JSON.stringify(want);
    if (g !== w) { console.error(`FAIL ${label}: got ${g} want ${w}`); fail = 1; }
};

/* GROUP: kind and extra_keys declared on one member, role on another. */
const grp = mk({
    a: { key: "a", viz: { group: "g", role: "r1", kind: "custom:t", extra_keys: ["off1", "off2"] } },
    b: { key: "b", viz: { group: "g", role: "r2" } },
});
const gr = resolveViz({ keys: ["a", "b"], metaIndex: grp }).groups;
check("group count", gr.length, 1);
check("group spans both cells", gr[0].keys, ["a", "b"]);
check("group extraKeys", gr[0].extraKeys, ["off1", "off2"]);

/* SINGLE: the path that always worked, pinned so a fix cannot regress it. */
const sng = mk({ s: { key: "s", viz: { kind: "custom:t", extra_keys: ["off3"] } } });
const sr = resolveViz({ keys: ["s"], metaIndex: sng }).groups;
check("single extraKeys", sr[0].extraKeys, ["off3"]);

if (fail) process.exit(1);
console.log("ok - viz groups carry extra_keys");
'
