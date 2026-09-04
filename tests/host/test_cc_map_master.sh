#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE MASTER FX CC MAP, and the naming both maps share.
#
# Master FX is not a chain -- each position is a standalone audio-FX plugin --
# so none of the chain's machinery applies and this is a second implementation
# of the same idea. That is exactly why it needs its own test: the two must
# agree about page order, page names and how a row reads, or the same module
# reads one way in a slot and another on the master bus.
#
# Each assertion below corresponds to something that shipped wrong at least
# once: rows in declaration order rather than page order, four rows called
# "Gain", SHOUTING names, and a Clear all offered when there was nothing to
# clear.
#
# NO APOSTROPHES BELOW THIS LINE inside the node script: it is a single-quoted
# bash string, and one apostrophe ends it early with an error pointing nowhere
# near the real line.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the master cc map tests" >&2
  exit 1
fi

node --input-type=module -e '
import { masterGridHierarchy, allMasterGridParams, masterCcMapLevels,
         prettyName, ccMapMenu, MASTER_CC_CONTROL_KEY }
  from "./src/shadow/shadow_ui_slot_grid.mjs";
import { planPages } from "./src/shared/param_pages/page_plan.mjs";
import { step } from "./src/shared/param_pages/page_nav.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* An EQ with the four-Gain problem, one parameter assigned. */
const ROWS = [{
  slot: 0, module: "4k-eq",
  params: [
    { key: "lf_gain",  name: "GAIN", group: "LF",  cc: 1,  min: -15, max: 15, type: "int" },
    { key: "lm_gain",  name: "GAIN", group: "LMF", cc: -1, min: -15, max: 15, type: "int" },
    { key: "hpf",      name: "HPF",  group: "LF",  cc: -1, min: 0,   max: 1,  type: "float" },
  ],
}];

/* ---- naming ---- */
ok(prettyName("GAIN") === "Gain", "a four-letter all-caps word is title-cased");
ok(prettyName("HPF") === "HPF", "a three-letter acronym is left alone");
ok(prettyName("LMF GAIN") === "LMF Gain", "...even next to one that is not");
ok(prettyName("Band 1 Gain") === "Band 1 Gain", "already-cased names are untouched");

/* ---- rows ---- */
const lv = masterCcMapLevels(ROWS, "Ch 16");
const rows = lv.mccmap_fx0.menu;
ok(rows[0].label === "LF Gain", "the page name disambiguates identical parameters");
ok(rows[1].label === "LMF Gain", "...for each page");
ok(rows[0].value === "1 Ch 16", "an assigned row shows CC and channel");
ok(rows[1].value === "--", "an unassigned row shows nothing, and is still listed");
ok(rows[2].label === "LF HPF", "an acronym survives the page prefix");

/* ---- clear all ---- */
ok(rows[rows.length - 1].label === "Clear all",
   "Clear all is offered when something is assigned");
const none = masterCcMapLevels(
  [{ slot: 0, module: "eq", params: [{ key: "a", name: "A", group: "", cc: -1 }] }], "All");
ok(none.mccmap_fx0.menu.every((r) => r.label !== "Clear all"),
   "...and not when there is nothing to clear");

/* ---- the index ---- */
ok(lv.ccmap.menu[0].label === "4k-eq", "the index names the module");
ok(lv.ccmap.menu[0].value === "1/3", "...and counts what is assigned");
ok(lv.ccmap.menu[0].level === "mccmap_fx0", "...and navigates to it");
ok(lv.mccmap_fx0.hidden === true, "a component page is hidden from the walk");

/* ---- the gate, and the walk ---- */
const h = masterGridHierarchy(false, ROWS, "Ch 16");
ok(h.levels.midi.knobs[0] === MASTER_CC_CONTROL_KEY,
   "Master FX has a CC on/off switch of its own");
ok(h.levels.root.params.filter((p) => p.level).map((p) => p.level).join(",")
   === "lfo1,lfo2,actions,midi", "MIDI comes after Actions");
ok(allMasterGridParams().some((p) => p.key === MASTER_CC_CONTROL_KEY),
   "...and it is declared, or the grid cannot draw it");

const planned = planPages({ hierarchy: h, chainParams: allMasterGridParams() });
const pages = planned.pages;
const idx = pages.findIndex((p) => p && p.level === "ccmap");
ok(idx >= 0, "the index page is planned");
ok(pages.findIndex((p) => p && p.level === "mccmap_fx0") >= 0,
   "a page reached only from a menu row is planned too");
ok(step(pages, idx, 1) === idx, "the jog does not walk into the hidden page");

/* ---- both maps agree ---- */
const slotRow = ccMapMenu("1|fx1|lf_gain|GAIN|LF;")[0];
ok(slotRow.label === rows[0].label,
   "a slot and the master bus render the same parameter identically");

process.exit(fail ? 1 : 0);
'
