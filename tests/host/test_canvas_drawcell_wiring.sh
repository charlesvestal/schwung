#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A WIDGET MUST NOT OUTLIVE ITS MODULE, AND THE IMPORT MUST WORK ON DEVICE.
#
# The registry is process-global and shadow_ui is long-lived, so a widget
# registered by slot 1s module would still be registered after that module was
# swapped out -- and a later module declaring the same custom: name would
# silently inherit the wrong art. resetCanvasState clears it.
#
# The import path is pinned because getting it wrong is INVISIBLE HERE and fatal
# on hardware: shadow_ui.js reaches every shared/param_pages module by the
# absolute device path /data/UserData/schwung/shared/param_pages/..., never by a
# relative one. A relative import resolves fine on a Mac checkout and cannot
# resolve on the device.
#
# Source-level pins, because shadow_ui.js cannot be imported without the host
# bindings. Weaker than unit tests; the drawing behaviour itself is covered by
# test_widget_one_strike.sh, which exercises the real registry.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the drawCell wiring checks" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const src = readFileSync("./src/shadow/shadow_ui.js", "utf8");

/* Assertions about what code DOES must not be satisfied -- or defeated -- by
 * what a comment SAYS. The comment in resetCanvasState explaining why it must
 * not call clearWidgets() contains the literal "clearWidgets(", and tripped the
 * check that it does not call it. Strip comments before asserting on a body. */
const code = (s) => s.replace(/\/\*[\s\S]*?\*\//g, " ").replace(/\/\/[^\n]*/g, " ");

/* The import must be the absolute device path, like every other
 * shared/param_pages import in this file. */
ok(/from\s+.\/data\/UserData\/schwung\/shared\/param_pages\/widget_registry\.mjs./.test(src),
   "widget_registry is imported by its absolute device path");
ok(!/from\s+.[.][.]?\/shared\/param_pages\/widget_registry\.mjs./.test(src),
   "widget_registry is NOT imported by a relative path (fatal on device)");
for (const fn of ["registerWidget", "clearWidgets", "setWidgetLogger"]) {
  ok(new RegExp("\\b" + fn + "\\b").test(src), `shadow_ui imports ${fn}`);
}

/* Registration is guarded on BOTH the hook and the kind. */
ok(/typeof\s+\w+\.drawCell\s*===\s*"function"/.test(src),
   "a non-function drawCell is ignored rather than registered");
ok(/typeof\s+\w+\.widgetKind\s*===\s*"string"/.test(src),
   "an overlay with no widgetKind string is ignored");

/* WIDGET LIFETIME IS THE COMPONENTS, NOT THE CANVAS VIEWS.
 *
 * This is the defect the POC found. Registration used to live at the overlay
 * load inside openCanvasPreview, which only runs when the user CLICKS a
 * type:"canvas" param -- so an in-grid widget did not appear until the
 * fullscreen view had been opened once, vanished on the way out (open AND close
 * both call resetCanvasState, which cleared the registry), and never appeared at
 * all for a module with no canvas param.
 *
 * Ordering is exactly what a source-level test cannot see, so what is pinned
 * here is the STRUCTURE that made the ordering wrong: which function owns the
 * lifetime, and which one must not touch it. */
const tdStart = src.indexOf("function resetCanvasState");
ok(tdStart > 0, "resetCanvasState exists");
const tdBody = code(src.slice(tdStart, src.indexOf("\nfunction ", tdStart + 1)));
ok(!/clearWidgets\s*\(/.test(tdBody),
   "resetCanvasState does NOT clear the registry (it runs on every canvas open AND close)");

const ecwStart = src.indexOf("function ensureComponentWidgets");
ok(ecwStart > 0, "ensureComponentWidgets exists");
const ecwBody = code(src.slice(ecwStart, src.indexOf("\nfunction ", ecwStart + 1)));
ok(/clearWidgets\s*\(\s*\)/.test(ecwBody),
   "ensureComponentWidgets owns the clear");
ok(/registerWidget\s*\(/.test(ecwBody),
   "ensureComponentWidgets owns the registration");
ok(/widgetModuleLoaded/.test(ecwBody),
   "it is a no-op when the module has not changed, so it is safe on a hot path");

/* And the canvas-open path must not register. */
const ocpStart = src.indexOf("function openCanvasPreview");
const ocpBody = code(src.slice(ocpStart, src.indexOf("\nfunction ", ocpStart + 1)));
ok(!/registerWidget\s*\(/.test(ocpBody),
   "openCanvasPreview does NOT register widgets");

/* AN UNRESOLVED MODULE ID MUST NOT LATCH.
 *
 * getHierarchyActiveModuleId ends in getSlotParam(...) || "" -- an IPC read that
 * has not settled on first entry. Recording "" as the loaded module id meant the
 * retry never happened and the widget appeared only after the user dived into
 * the fullscreen canvas and came back (which re-entered the editor and read
 * again). Same tri-state rule as everywhere else: a read that did not answer
 * must not become a cached verdict. */
ok(/if\s*\(\s*!id\s*\)\s*return;/.test(ecwBody),
   "an empty module id returns WITHOUT latching, so the question stays open");
const latchIdx = ecwBody.indexOf("widgetModuleLoaded =");
const guardIdx = ecwBody.indexOf("if (!id) return;");
ok(guardIdx >= 0 && guardIdx < latchIdx,
   "the empty-id guard comes BEFORE the latch assignment");

/* SAME RULE, ONE LAYER DOWN. chain_params is a read too, and an empty array is
 * an answer that has not arrived rather than "this module declares nothing" --
 * a chain component always declares something. Latching on the id alone and
 * then deciding from unsettled params is what shipped, twice: it returned
 * already-latched, the retry stopped, and only the editor-exit path (reached by
 * diving into the canvas and back) cleared it. Pin EVERY guard against the
 * latch, not just the one that was wrong last time. */
const cpGuardIdx = ecwBody.search(/chainParams\.length === 0\s*\)\s*return;/);
ok(cpGuardIdx >= 0, "an empty chainParams returns without deciding");
ok(cpGuardIdx < latchIdx, "the empty-chainParams guard also comes BEFORE the latch");

ok(/function tickComponentWidgets/.test(src), "there is a retry for the unresolved case");
const tcwStart = src.indexOf("function tickComponentWidgets");
const tcwBody = code(src.slice(tcwStart, src.indexOf("\nfunction ", tcwStart + 1)));
ok(/if\s*\(\s*widgetModuleLoaded\s*\)\s*return;/.test(tcwBody),
   "the retry stops once the id resolves");
ok(/WIDGET_RETRY_TICKS/.test(tcwBody),
   "the retry is THROTTLED -- the id costs an IPC read, ~2.8ms against a 1.68ms render");

/* THE RETRY MUST ASK THE VIEW THAT IS ON SCREEN.
 *
 * It read hierEditorChainParams and getHierarchyActiveModuleId while running
 * from the PARAM_PAGES tick, and logged id="" params=0 forever: entering
 * PARAM_PAGES tears the hierarchy editor down, so hierEditorSlot is -1 and
 * getHierarchyActiveModuleId returns "" on its first line without ever
 * performing a read, while hierEditorChainParams has been reset to []. Four
 * theories died on the assumption that an empty answer meant an unsettled
 * READ; it meant the wrong SOURCE. */
ok(/paramPagesSlot\(\)/.test(tcwBody) && /paramPagesComponent\(\)/.test(tcwBody),
   "the knob-grid retry takes its identity from the knob grid");
ok(!/hierEditor/.test(tcwBody),
   "the knob-grid retry does NOT read hierarchy-editor state, which PARAM_PAGES has torn down");
ok(/if \(view === VIEWS\.PARAM_PAGES\) tickComponentWidgets\(\);/.test(src),
   "the retry is driven from the knob-grid tick");

/* Leaving a component must end its widgets lifetime. */
ok(/widgetModuleLoaded = "";/.test(src), "the latch is cleared when the editor exits");

/* Registration is driven from where the CONTRACT becomes known. */
ok(/ensureComponentWidgets\s*\(\s*getHierarchyActiveModuleId\(\)\s*,\s*hierEditorChainParams\s*\)/.test(src),
   "it is called where a components chain_params are fetched");

/* The logger is installed, so a disabled widget is attributable. */
ok(/setWidgetLogger\s*\(/.test(src), "shadow_ui installs the widget logger");


process.exit(fail ? 1 : 0);
'
