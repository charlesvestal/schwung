#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A preset list opens on the preset the component is already on, not on its
# first row. The rule is one pure helper (cursorForPreset); the two lists that
# used to hard-code row 0 -- the My Presets browser (slot chain, Master FX,
# Send A, Send B) and the Master preset picker -- call it, and the My Presets
# "Load" action hands the browser the loaded preset's name.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const eq = (got, want, m) => { if (got !== want) fail(m + " -- got " + got + " want " + want); };

const { cursorForPreset } = await import(R + "/src/shared/param_pages/current_preset.mjs");

/* ---- the helper ---------------------------------------------------------- */
const names = ["Alpha", "Beta", "Gamma"];
eq(cursorForPreset(names, "Gamma"), 2, "lands on the loaded preset");
eq(cursorForPreset(names, "Alpha"), 0, "first row is a real answer, not a fallback");
eq(cursorForPreset(names, ""), 0, "no preset loaded opens at the top");
eq(cursorForPreset(names, null), 0, "null name opens at the top");
eq(cursorForPreset(names, undefined), 0, "missing name opens at the top");
eq(cursorForPreset(names, "Deleted"), 0, "a preset no longer on disk opens at the top");
eq(cursorForPreset([], "Alpha"), 0, "empty list opens at 0");
eq(cursorForPreset(undefined, "Alpha"), 0, "a list that failed to load opens at 0");
eq(cursorForPreset(names, "Beta", 0, 1), 2, "offset skips fixed rows ahead of the presets");
eq(cursorForPreset(names, "", 0, 1), 0, "no preset + offset still opens on the fixed row");
eq(cursorForPreset(names, "Nope", 0, 1), 0, "unknown + offset falls back, not to offset");
eq(cursorForPreset(["10", "2"], "2"), 1, "names compare as strings");

/* ---- wiring -------------------------------------------------------------- */
const ui = readFileSync(R + "/src/shadow/shadow_ui.js", "utf8");
const presets = readFileSync(R + "/src/shadow/shadow_ui_presets.mjs", "utf8");

function bodyOf(src, name) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone -- this test is anchored on it"); return ""; }
  const end = src.indexOf("\n}\n", at);
  return src.slice(at, end + 2);
}

const browser = bodyOf(presets, "enterPresetBrowser");
if (/selectedPreset\s*=\s*0\s*;/.test(browser)) fail("enterPresetBrowser must not reset the cursor to row 0");
if (!/selectedPreset\s*=\s*cursorForPreset\(/.test(browser)) fail("enterPresetBrowser must place the cursor with cursorForPreset");
if (!/o\.currentName/.test(browser)) fail("enterPresetBrowser must read currentName from its options");

const load = ui.slice(ui.indexOf("case \"up_load\":"), ui.indexOf("case \"up_save\":"));
if (!/currentName:\s*record\s*\?\s*record\.name/.test(load)) fail("up_load must pass the loaded preset name to the browser");

const picker = bodyOf(ui, "enterMasterPresetPicker");
if (/selectedMasterPresetIndex\s*=\s*0\s*;/.test(picker)) fail("the Master picker must not hard-code row 0");
if (!/cursorForPreset\([\s\S]*?currentMasterPresetName,\s*0,\s*1\)/.test(picker)) {
  fail("the Master picker must use cursorForPreset with offset 1 (row 0 is [New])");
}

/* RUN the picker entry: lift it and drive it with a loaded and an unloaded preset. */
const lifted = new Function(
  "loadMasterPresetList", "cursorForPreset", "announce",
  "state",
  picker
    .replace(/\bmasterPresets\b/g, "state.masterPresets")
    .replace(/\bcurrentMasterPresetName\b/g, "state.current")
    .replace(/\binMasterPresetPicker\b/g, "state.inPicker")
    .replace(/\bselectedMasterPresetIndex\b/g, "state.index")
    .replace(/\bneedsRedraw\b/g, "state.redraw")
  + "\nreturn enterMasterPresetPicker;");
function enter(current, list) {
  const state = { masterPresets: list.map((n, i) => ({ name: n, index: i })), current, inPicker: false, index: -1, redraw: false };
  const said = [];
  lifted(() => {}, cursorForPreset, (m) => said.push(m), state)();
  return { index: state.index, said: said[0], inPicker: state.inPicker };
}
let r = enter("Beta", ["Alpha", "Beta", "Gamma"]);
eq(r.index, 2, "Master picker opens on the loaded preset (row 0 is [New])");
eq(r.said, "Master Presets, Beta", "and announces it by name");
r = enter("", ["Alpha", "Beta"]);
eq(r.index, 0, "Master picker opens on [New] when nothing is loaded");
eq(r.said, "Master Presets, [New]", "and announces [New]");
r = enter("Gone", ["Alpha"]);
eq(r.index, 0, "Master picker opens on [New] when the loaded preset is no longer listed");

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.error("PASS: test_preset_list_cursor");
'
