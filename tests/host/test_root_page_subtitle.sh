#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE LANDING PAGE IS "Main", AND `subtitle` IS THE ONLY WAY PAST THAT.
#
# page_plan.mjs fixes the walk root's grid page to "Main" on purpose, so that
# 16 modules do not each open on their own word for "where you land". That
# leaves a module which splits one level per page unable to say which page the
# landing one IS -- and for a module with three identically-labelled ADSR rows
# the header is the only thing telling them apart.
#
# `subtitle` is opt-in for exactly that. This pins all three properties:
#   - absent  -> "Main" (the existing convention, unchanged for every module)
#   - present -> "Main - <subtitle>"
#   - a level `name` alone must NOT do it, or the 16 modules regress.

node --input-type=module -e '
import { planPages } from "./src/shared/param_pages/page_plan.mjs";

const mk = (root) => ({ levels: { root, other: { name: "Other", knobs: ["z"] } } });
const params = [
    { key: "a", name: "A", type: "float", min: 0, max: 1 },
    { key: "z", name: "Z", type: "float", min: 0, max: 1 },
];
const plan = (root) => planPages({ hierarchy: mk(root), chainParams: params });
const nameOfFirst = (p) => (p.pages.find((q) => q.kind === "knobs") || {}).name;

let fail = 0;
const eq = (label, got, want) => {
    if (got !== want) { console.error(`FAIL ${label}: got ${JSON.stringify(got)} want ${JSON.stringify(want)}`); fail = 1; }
};

eq("no subtitle",      nameOfFirst(plan({ knobs: ["a"] })), "Main");
eq("name is not one",  nameOfFirst(plan({ name: "Patch", knobs: ["a"] })), "Main");
eq("subtitle applies", nameOfFirst(plan({ name: "Patch", subtitle: "OP2", knobs: ["a"] })), "Main - OP2");
eq("blank ignored",    nameOfFirst(plan({ subtitle: "   ", knobs: ["a"] })), "Main");

if (fail) process.exit(1);
console.log("ok - root page subtitle");
'
