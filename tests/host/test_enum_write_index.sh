#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# AN ENUM WRITE MUST NOT RESOLVE AN INDEX AS AN OPTION NAME.
#
# formatParamForSet used to try options.indexOf(String(rawValue)) FIRST, for
# any value. Every caller in this repo hands it knobStep() output -- an index,
# a number -- so an index whose numeral appeared among the options resolved to
# that option's POSITION instead. Picking "-100" out of
# ["-100","-50","0","+50","+100"] wrote 2, because "0" sits at index 2.
#
# It is silent: the cell goes on showing the value the grid believes it wrote.
#
# This test is built from the REAL fleet contract rather than invented cases,
# so it measures the thing that actually shipped. It asserts three properties:
#
#   1. every declared enum in the fleet round-trips index -> wire -> index
#   2. the two shapes that provably broke are pinned by name
#   3. an enum a plugin wires by NAME still accepts both an index and a label,
#      which is what the label branch was added for and must keep doing

node --input-type=module -e '
import fs from "node:fs";
const { formatParamForSet } = await import("./src/shared/param_format.mjs");

let bad = 0;
const fail = (m) => { console.error("FAIL " + m); bad++; };

/* 1. Every enum in the fleet fixture. */
const raw = fs.readFileSync("tests/fixtures/module-contracts.json", "utf8");
let enums = 0, checked = 0;
(function walk(n) {
    if (Array.isArray(n)) { n.forEach(walk); return; }
    if (!n || typeof n !== "object") return;
    if (n.type === "enum" && Array.isArray(n.options) && n.options.length) {
        enums++;
        /* Only index-wired enums write a bare index; a names-wired one writes
         * the name, which is checked separately below. */
        if (!n.options_as_string && n.wire_format !== "name") {
            for (let i = 0; i < n.options.length; i++) {
                const wire = formatParamForSet(i, n);
                if (Number(wire) !== i) {
                    fail("index " + i + " of " + (n.key || "?") +
                         " [" + n.options.slice(0, 6).join(",") + "] wrote " + wire);
                    i = n.options.length;
                }
                checked++;
            }
        }
    }
    for (const v of Object.values(n)) walk(v);
})(JSON.parse(raw));
if (enums < 100) fail("fixture yielded only " + enums + " enums - is it still the fleet contract?");

/* 2. The two shapes that provably broke. */
const offset = { type: "enum", options: ["-100", "-50", "0", "+50", "+100"] };
if (formatParamForSet(0, offset) !== "0") {
    fail("minijv lfo1offset shape: picking -100 wrote " + formatParamForSet(0, offset));
}
const chan = { type: "enum", options: Array.from({ length: 16 }, (_, i) => String(i + 1)) };
for (let i = 0; i < 16; i++) {
    if (Number(formatParamForSet(i, chan)) !== i) {
        fail("channel shape: index " + i + " wrote " + formatParamForSet(i, chan));
        break;
    }
}

/* 3. The label branch still serves what it was added for. */
const named = { type: "enum", options: ["Sine", "Saw"], options_as_string: true };
if (formatParamForSet(1, named) !== "Saw") fail("names-wired enum lost the index path");
if (formatParamForSet("Saw", named) !== "Saw") fail("names-wired enum lost the label path");

if (bad) process.exit(1);
console.log("PASS: enum writes resolve an index as an index (" + enums +
            " fleet enums, " + checked + " index round trips)");
'
