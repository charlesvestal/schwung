#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A "Head: Tail" VALUE GIVES UP ITS HEAD FIRST.
#
# An LFO routing reads "Mini-JV: Cutoff". Cut from the end, a long module name
# (minijv used to answer `name` with its PATCH) pushed the param -- the thing
# being chosen -- off the header and off the enum list row. fitHeadTail
# shortens the head, measured through the ctx that draws it (the device face
# and movy 5px face differ), and the enum list opts in.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
globalThis.fill_rect = () => {}; globalThis.print = () => {};
globalThis.set_pixel = () => {}; globalThis.text_width = (t) => String(t).length * 6;
const R = process.cwd();
const { fitHeadTail } = await import(R + "/src/shared/param_pages/render_page.mjs");
const { drawEnumList } = await import(R + "/src/shared/param_pages/enum_list.mjs");

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };
const narrow = { textWidth: (t) => String(t).length * 4 };
const wide = { textWidth: (t) => String(t).length * 6 };
const LONG = "Grand Piano Layered: Cutoff";

if (fitHeadTail(narrow, LONG, 200) !== LONG) bad("text that fits must be untouched");
const n = fitHeadTail(narrow, LONG, 64);
if (!/: Cutoff$/.test(n) || narrow.textWidth(n) > 64) bad("the param must survive, got " + n);
const w = fitHeadTail(wide, LONG, 64);
if (!/: Cutoff$/.test(w) || wide.textWidth(w) > 64) bad("measured per ctx: wide face got " + w);
if (n === w) bad("the two faces should fit different amounts of head");
if (fitHeadTail(narrow, "Cutoff Frequency", 40) !== "Cutoff Fre")
  bad("no separator: ordinary cut, got " + fitHeadTail(narrow, "Cutoff Frequency", 40));

/* The enum list: every visible row keeps its param. */
const printed = [];
const ctx = { fillRect() {}, print: (x, y, t) => printed.push(String(t)), textWidth: (t) => String(t).length * 5 };
const opts = ["None", "Grand Piano Layered: Cutoff", "Grand Piano Layered: Resonance", "LFO 2: Depth"];
drawEnumList(ctx, { title: "Targ", headerRight: "TURNING", options: opts, index: 1, markIndex: 0,
                    footer: [["BACK", "EXIT"]] });
for (const want of ["Cutoff", "Resonance", "Depth"]) {
  if (!printed.some((t) => t.endsWith(": " + want)))
    bad("the list row for " + want + " lost its param: " + JSON.stringify(printed));
}

if (fail) process.exit(1);
console.log("PASS: a Head: Tail value keeps its tail, measured per face; the enum list opts in");
'
