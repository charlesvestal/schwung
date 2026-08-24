#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Audition is ONE switch with TWO consumers.
#
# Global Settings -> Audition (stored key `browser_preview`) gates the file
# browser playing a highlighted WAV, and the User Presets list applying the
# highlighted preset to the live slot. Both are "hear it before you pick it".
#
# It is default OFF, and that is load-bearing in two directions:
#
#  - auditioning a preset APPLIES state to the live slot, and the presets list
#    is now a page at the end of every component rather than an indented row
#    inside a picker, so it is far easier to land on by accident
#  - the DECLARED default and the IN-MEMORY initial value must agree. The
#    settings screen reads the variable, not the contract, so a device with no
#    stored config would draw "On" while the contract says "Off" -- a
#    disagreement no test would catch and no user could explain.
#
# The stored key stays `browser_preview`: renaming it would silently discard
# every existing choice, because the toggle is the only thing that persists it.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node -e '
const fs = require("fs");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (m) => { console.error("ok: " + m); };

const grid = fs.readFileSync("src/shadow/shadow_ui_global_grid.mjs", "utf8");
const ui = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const presets = fs.readFileSync("src/shadow/shadow_ui_presets.mjs", "utf8");

/* ---- 1. the contract declares it, default OFF, key unchanged ------------ */
const row = grid.match(/bool\(\s*"browser_preview"\s*,\s*"([^"]*)"\s*,\s*(\d)\s*\)/);
if (!row) {
  fail("browser_preview is no longer declared as a bool row in the global grid");
} else {
  if (row[2] === "0") ok("Audition is declared default OFF");
  else fail("Audition must default OFF, contract says " + row[2]);
  ok("stored key is still browser_preview (label: \"" + row[1] + "\")");
}

/* ---- 2. the in-memory initial value AGREES with the declared default ---- */
/* This is the drift the header warns about: the settings screen renders from
   the variable, so if these disagree a fresh device shows the wrong state. */
const decl = ui.match(/let\s+previewEnabled\s*=\s*(true|false)\s*;/);
if (!decl) {
  fail("could not find the previewEnabled declaration in shadow_ui.js");
} else if (row) {
  const declaredOn = row[2] === "1";
  const initialOn = decl[1] === "true";
  if (declaredOn === initialOn) {
    ok("in-memory initial (" + decl[1] + ") agrees with the declared default (" + row[2] + ")");
  } else {
    fail("DRIFT: contract default is " + row[2] + " but previewEnabled initialises to " +
         decl[1] + " -- a fresh device would draw the opposite of what it does");
  }
}

/* ---- 3. only the toggle persists it -------------------------------------- */
/* What lets the default move at all: a device that never chose follows the new
   default, one that chose keeps its choice. A second writer would pin every
   existing install to whatever it booted with, forever. Same rule as
   param_view, which documents this in CLAUDE.md. */
/* Count CALLS, not the definition -- `function saveBrowserPreviewConfig()`
   matches a naive /saveBrowserPreviewConfig\(\)/ too. */
const savers = (ui.match(/(?<!function\s)saveBrowserPreviewConfig\(\)/g) || []).length;
if (savers === 1) ok("saveBrowserPreviewConfig has exactly one call site (the toggle)");
else fail("expected exactly 1 CALL to saveBrowserPreviewConfig, found " + savers +
          " -- a second writer freezes the default for every existing device");

/* ---- 4. the presets list is gated on it, through ctx -------------------- */
if (/ctx\.auditionEnabled/.test(presets)) {
  ok("the presets list reads the gate through ctx, not a host global");
} else {
  fail("shadow_ui_presets.mjs does not consult ctx.auditionEnabled");
}
if (/_ctx\.auditionEnabled\s*=/.test(ui)) ok("shadow_ui.js exposes ctx.auditionEnabled");
else fail("shadow_ui.js does not expose ctx.auditionEnabled");

/* The gate must gate the CAPTURE, not just the apply: with audition off there
   should be no reason to read the state blob on entry at all (a param read is
   ~2.8ms against a 1.68ms whole-page render). */
/* Anchor on the FUNCTION, not on a bare assignment -- `originalState = null;`
   also matches the module-level declaration hundreds of lines earlier, which
   would slice in a different getSlotStateWithRetry and make this pass or fail
   for the wrong reason. */
const entryAt = presets.indexOf("export function enterPresetBrowser(");
if (entryAt < 0) fail("enterPresetBrowser is gone -- this test is anchored on it");
const entryEnd = presets.indexOf("\n}\n", entryAt);
const entry = entryAt >= 0 ? presets.slice(entryAt, entryEnd) : "";
const gateAt = entry.indexOf("auditionEnabled");
const readAt = entry.indexOf("getSlotStateWithRetry");
if (gateAt >= 0 && readAt >= 0 && gateAt < readAt) {
  ok("the gate is decided BEFORE the state read, so audition-off costs no IPC on entry");
} else {
  fail("the audition gate does not precede the :state read on entry -- audition-off " +
       "should not pay for a read it will not use");
}

/* ---- 5. off disables the AUDITION, not the list ------------------------- */
/* Load must still load. If the gate had been put on the list itself, turning
   audition off would have removed the feature rather than its side effect. */
if (/function\s+applyPreset|ctx\.onPresetLoaded/.test(presets)) {
  ok("an explicit Load path still exists independently of the preview gate");
} else {
  fail("could not find the explicit Load path -- did the gate disable the list itself?");
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS");
'
