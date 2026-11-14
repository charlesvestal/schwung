#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# [New Slot Preset] is a deliberate empty, and autosave must be told so.
#
# autosaveOneSlot refuses to overwrite a saved chain with "{}" when the slot
# is empty but still has a name, unless slotUserCleared[i] is set -- that is
# its guard against a boot-time load failure. The chain editor's None pick
# sets the flag; applyPatchSelection did not, and renames the slot "Untitled"
# (a non-empty name), so the guard kept the old slot_N.json and the removed
# chain came back on the next set load.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const patches = readFileSync("src/shadow/shadow_ui_patches.mjs", "utf8").split(String.fromCharCode(13)).join("");
const ui = readFileSync("src/shadow/shadow_ui.js", "utf8").split(String.fromCharCode(13)).join("");

const at = patches.indexOf("export function applyPatchSelection(");
const end = patches.indexOf("\n}\n", at);
if (at < 0 || end < 0) { fail("applyPatchSelection not found"); process.exit(1); }
const body = patches.slice(at, end + 2).replace("export function", "function");

const run = (patchName) => {
  const cleared = [false, false, false, false];
  const ctx = {
    selectedSlot: 2, slots: [{}, {}, { name: "Old" }, {}],
    patches: [{ name: "[New Slot Preset]" }, { name: "Pad" }],
    selectedPatch: patchName === "[New Slot Preset]" ? 0 : 1,
    saveSlotsToConfig: () => {}, fetchKnobMappings: () => {},
    invalidateKnobContextCache: () => {}, setView: () => {}, VIEWS: {},
    setSlotUserCleared: (s, v) => { cleared[s] = !!v; },
    getSlotParam: () => "x",
  };
  cleared[2] = patchName !== "[New Slot Preset]";
  new Function("ctx", "PATCH_INDEX_NONE", "fetchPatchDetail",
    body + "\nreturn applyPatchSelection;")(ctx, 65535, () => {})();
  return cleared[2];
};

if (run("[New Slot Preset]") !== true) fail("New Slot Preset must set slotUserCleared");
if (run("Pad") !== false) fail("loading a real preset must clear slotUserCleared");

if (!/_ctx\.setSlotUserCleared\s*=\s*\([^)]*\)\s*=>\s*\{\s*slotUserCleared\[slot\]/.test(ui))
  fail("shadow_ui.js must wire ctx.setSlotUserCleared to the slotUserCleared array");

if (!/function doDeletePreset[\s\S]*?slotUserCleared\[slotIndex\]\s*=\s*true[\s\S]*?shadow_request_patch\(slotIndex, PATCH_INDEX_NONE\)/.test(ui))
  fail("doDeletePreset must set slotUserCleared before clearing the slot");

if (failures) process.exit(1);
console.log("PASS: New Slot Preset marks the slot user-cleared");
'
