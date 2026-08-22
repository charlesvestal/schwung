#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Static checks on the shadow_ui.js side of the knob-grid preview.
#
# shadow_ui.js cannot be executed off-device, so this does the two things that
# CAN be verified without hardware: it parses the file as a module (catching any
# syntax error introduced by wiring edits) and pins the wiring itself — that the
# view is dispatched, ticked, fed MIDI, and that the hand-off back to the list
# editor cannot loop.
#
# The loop is the one that matters. The grid hands an opaque param to the list
# by calling enterHierarchyEditor, which checks the Param View setting and would
# bounce straight back into the grid, forever, hanging the UI. A one-shot guard
# breaks it, and this test exists so nobody removes the guard without noticing.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the wiring tests" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# 1. It still parses. Shadow UI is ESM; a stray brace here takes out every screen.
cp src/shadow/shadow_ui.js "$TMP/shadow_ui.mjs"
if ! node --check "$TMP/shadow_ui.mjs" 2>"$TMP/err"; then
  echo "FAIL: shadow_ui.js does not parse:"; cat "$TMP/err"; exit 1
fi
cp src/shadow/shadow_ui_param_pages.mjs "$TMP/view.mjs"
if ! node --check "$TMP/view.mjs" 2>"$TMP/err"; then
  echo "FAIL: shadow_ui_param_pages.mjs does not parse:"; cat "$TMP/err"; exit 1
fi

node -e '
const fs = require("fs");
const s = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const v = fs.readFileSync("src/shadow/shadow_ui_param_pages.mjs", "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
const want = (re, what, src) => { if (!(re).test(src || s)) fail(what); };

/* ---- the view exists and is reachable -------------------------------- */
want(/PARAM_PAGES:\s*"parampages"/, "VIEWS.PARAM_PAGES is not declared");
want(/from '\''\.\/shadow_ui_param_pages\.mjs'\''/, "the view module is not imported");
/* The view must be drawn from the MAIN render switch, not only from the co-run
 * one. shadow_ui.js has five `switch (view)` blocks and two of them contain a
 * line reading `case VIEWS.HIERARCHY_EDITOR: drawHierarchyEditor(); break;` —
 * an earlier version of this wiring landed the case in dispatchCoRunDraw() and
 * this test passed anyway, because it only asked whether the case existed
 * ANYWHERE. On device that meant the view was entered, the controller ran and
 * announced pages, and nothing was ever drawn. The main switch lives inside
 * globalThis.tick, so the case has to appear after that definition. */
{
  const drawCases = [...s.matchAll(/case VIEWS\.PARAM_PAGES:/g)].map((m) => m.index);
  if (!drawCases.length) fail("the view is never drawn");
  const tickAt = s.indexOf("globalThis.tick = function()");
  if (tickAt < 0) fail("could not locate globalThis.tick to check the render switch");
  if (!drawCases.some((i) => i > tickAt)) {
    fail("PARAM_PAGES is only drawn from the co-run dispatcher — the main render " +
         "switch inside globalThis.tick has no case, so entering the view draws nothing");
  }
  if (!/case VIEWS\.PARAM_PAGES:[\s\S]{0,120}drawParamPages\(\)/.test(s)) {
    fail("the PARAM_PAGES case does not call drawParamPages");
  }
}
want(/if \(view === VIEWS\.PARAM_PAGES\) tickParamPages\(\)/, "the view is never ticked — reads would never happen");
want(/view === VIEWS\.PARAM_PAGES && paramPagesActive\(\)[\s\S]{0,200}handleParamPagesMidi\(data\)/,
     "the view never receives MIDI");

/* ---- the setting is real, defaults to the list, and persists ---------- */
want(/key: "param_view"[\s\S]{0,120}options: \["List", "Knobs"\]/, "the Param View setting is not in the menu");
/* The DEFAULT itself, and the migration around it, now live in
 * test_param_view_default.sh -- it flipped to the knob grid once the grid could
 * draw everything the list could (mode selectors, child levels, enum pickers,
 * and a fleet fixture recaptured at 95 modules to prove the coverage).
 *
 * Kept here as a wiring check only: the setting must still be a real, readable
 * global. Pinning the VALUE in two places is how a deliberate change turns into
 * a CI failure in a file nobody was looking at. */
want(/let paramViewGlobal = [01];/, "the Param View global is gone — the view module reads it through param_view_get_mode");
want(/globalThis\.param_view_get_mode/, "the view module reads the setting through a global that is not defined");
want(/function saveParamViewConfig/, "the setting does not persist");
want(/loadParamViewConfig\(\);/, "the setting is never loaded at init");

/* ---- the hand-off cannot loop ---------------------------------------- */
want(/suppressParamPagesOnce = true;\s*\n\s*enterHierarchyEditor\(/,
     "the grid hands off to the list without setting the anti-loop guard");
want(/paramPagesEnabled\(\) && !suppressParamPagesOnce/,
     "list entry ignores the anti-loop guard — handing off would bounce back into the grid forever");
want(/suppressParamPagesOnce = false;/, "the guard is never cleared, so the grid would stay disabled");

/* ---- BOTH chain editors honour the setting ---------------------------- *
 *
 * Param View = Knobs was silently slot-chain-only: enterHierarchyEditorWith
 * had the gate and enterMasterFxHierarchyEditor did not, so the same module
 * opened the labelled knob grid in a slot and the scrolling list on the master
 * bus. Reported from the device the day after the knob card shipped, and the
 * same drift section 1b of the Master FX variable-length design exists to end.
 *
 * Checked INSIDE the master entry point, not as a file-wide count: a second
 * occurrence anywhere would satisfy a count while the master screen went on
 * ignoring the setting.
 */
{
  const at = s.indexOf("function enterMasterFxHierarchyEditor(");
  if (at < 0) fail("enterMasterFxHierarchyEditor is gone");
  const end = s.indexOf("\n}\n", at);
  const body = s.slice(at, end < 0 ? s.length : end);
  if (!/paramPagesEnabled\(\) && !suppressParamPagesOnce/.test(body))
    fail("Master FX ignores the Param View setting - it always opens the hierarchy list");
  if (!/enterParamPages\(/.test(body))
    fail("Master FX never opens the knob grid");
  if (!/paramPagesChromeFor\(/.test(body))
    fail("Master FX opens the grid with no chrome - it would say S1, read the slot " +
         "spelling of the module key, and send Back to the slot chain editor");
  if (!/suppressParamPagesOnce = false;/.test(body))
    fail("the master entry point never clears the anti-loop guard, so the grid would " +
         "stay disabled after one hand-off");
}
/* And a Master FX component reaching the LIST editor must be routed to the
 * master entry point: the generic path sets hierEditorIsMasterFx = false, and
 * that flag is what decides where Back goes. */
want(/masterFxIndexFromComponentKey\(componentKey\)[\s\S]{0,160}enterMasterFxHierarchyEditor\(/,
     "enterHierarchyEditor does not route a Master FX component key to the master " +
     "entry point - Back would eject into the slot chain editor");

/* ---- the screen reader keeps the list --------------------------------- */
want(/tts_get_enabled[\s\S]{0,80}return false/, "the screen reader does not force the list", v);

/* ---- the view module owns no screens it should not --------------------- */
if (/openTextEntry|filepathBrowser|drawCanvas/.test(v)) {
  fail("the view module is reimplementing an editor the list already has");
}
/* The grid owns KNOBS, MENU, PRESET and ITEMS — all four are drawn in its
 * chrome. MODES and CHILD still belong to the list editor, and the view must
 * not start quietly drawing them. */
if (/PAGE_MODES|PAGE_CHILD/.test(v)) {
  fail("the view module is drawing page kinds it should hand to the list");
}

/* ---- device keys are built from the PREFIX, never the component key -----
 *
 * The component is `midiFx`; its params live under `midi_fx1` (see
 * getComponentParamPrefix in shadow_ui.js). Interpolating the component into a
 * device key asks for `midiFx_module` / `midiFx:is_loading`, which nobody
 * serves — and it fails SILENTLY, as a header reading "--" and an is_loading
 * probe that never returns true. Both bugs shipped that way.
 */
{
  const bad = v.match(/\$\{currentComponent\}(_module|:)/g);
  if (bad) {
    fail("the view builds a device param key from currentComponent (" + bad.join(", ") +
         ") — for midiFx that key does not exist; use currentPrefix");
  }
}

/* ---- an LFO target is never printed as its stored keys ------------------
 *
 * The routing is two internal keys and every surface used to print them raw:
 * "fx1:room_size" in the list row and the title, "FX" in a 30px grid cell.
 * The resolver exists now (shared/lfo_target_label.mjs) and the grid is wired
 * to it through describeTarget; the list must not drift back.
 */
want(/from '\''\/data\/UserData\/schwung\/shared\/lfo_target_label\.mjs'\''/,
     "the LFO target resolver is not imported");
want(/describeTarget:/, "the slot grid is not given a target resolver");
if (/target \+ ":" \+ param/.test(s)) {
  fail("a surface still prints the LFO target as its stored keys (target + \":\" + param)");
}

/* ---- the chain shape is DERIVED, never listed ---------------------------
 *
 * The editor list must come from the chain model, and it must come from THE
 * SLOT: a shared constant computed once at load is the fixed shape wearing a
 * derivation, and it would silently show the chain of one slot in the editor
 * of another.
 *
 * The negative checks below are the weak half — they only know the exact text
 * that was deleted, so a literal in single quotes or built by a hand-rolled
 * loop walks straight past them. The load-bearing half is the re-derivation in
 * the second node block, which runs the real function against the real model.
 */
if (/const CHAIN_COMPONENTS = \[\s*\{ key: "midiFx"/.test(s)) {
  fail("CHAIN_COMPONENTS is still a literal - it must come from chain_model");
}
if (/const CHAIN_COMPONENTS = chainEditorComponents\(/.test(s)) {
  fail("the editor list is computed once for every slot - it must be per-slot now");
}
want(/function slotChainComponents\(slotIndex\) \{\s*\n\s*return chainEditorComponents\(chainConfigs\[slotIndex\]/,
     "slotChainComponents does not derive from the config of that slot");
/* Importing the module is not enough: pulling in emptyChain alone would
 * satisfy a bare path match while the list went back to being hand-written. */
want(/\{[^}]*chainComponents[^}]*\}\s*\n?\s*from [^\n]*chain_model\.mjs/,
     "shadow_ui.js does not import chainComponents from the chain model");

console.log("PASS: shadow wiring — parses, view dispatched/ticked/fed, setting is wired, " +
            "hand-off cannot loop, screen reader keeps the list, device keys use the prefix, " +
            "LFO targets resolve to names");
'

# 2. The editor list, re-derived and compared entry by entry.
#
# Everything above is a regex over the source; this RUNS the thing. It lifts
# chainEditorComponents out of shadow_ui.js and calls it with the real
# chain_model, so a change to the model, to the derivation, or to the position
# counts all land here rather than on the device.
#
# What it protects is the screen: the label under the boxes, the picker header
# ("S1 > FX 2") and every announcement read the label and the ORDER off this
# list, and neither is visible in a diff of the derivation itself.
#
# When the chain genuinely grows, this test fails and the expectation is
# updated deliberately — a chain that changes shape without anyone editing a
# test is exactly what should not happen.
node --input-type=module -e '
import { readFileSync } from "node:fs";
import { chainComponents } from "./src/shared/chain_model.mjs";
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
const s = readFileSync("src/shadow/shadow_ui.js", "utf8");

const at = s.indexOf("function chainEditorComponents(cfg, caps) {");
if (at < 0) fail("chainEditorComponents is gone - the editor list is no longer derived");
const end = s.indexOf("\n}\n", at);
if (end < 0) fail("could not find the end of chainEditorComponents");
const src = s.slice(at, end + 2);

/* The function closes over exactly one name, so it is its parameter here —
 * no other part of shadow_ui.js is needed to run it. */
const derive = new Function("chainComponents",
                            src + "\nreturn chainEditorComponents;")(chainComponents);
const mod = (id) => ({ module: id, params: {} });
const shape = (l) => l.map((c) => ({ key: c.key, label: c.label, kind: c.kind, position: c.position }));
const CFG = { midiFx: [mod("arp")], synth: mod("sf2"), fx: [mod("freeverb"), mod("cloudseed")] };
const got = shape(derive(CFG));

/*
 * Seven positions, not five: the two `+` boxes are part of the editor list
 * now, because with no fixed empty positions left to click they are the only
 * way to lengthen a chain. Patch is still absent — it is the selection at -1.
 */
const want_ = [
  { key: "add_midi", label: "Add MIDI FX", kind: "add",      position: 0 },
  { key: "midiFx",   label: "MIDI FX",     kind: "module",   position: 1 },
  { key: "synth",    label: "Synth",       kind: "synth",    position: 2 },
  { key: "fx1",      label: "FX 1",        kind: "module",   position: 3 },
  { key: "fx2",      label: "FX 2",        kind: "module",   position: 4 },
  { key: "add_fx",   label: "Add FX",      kind: "add",      position: 5 },
  { key: "settings", label: "Settings",    kind: "settings", position: 6 },
];
if (JSON.stringify(got) !== JSON.stringify(want_)) {
  fail("the derived chain editor list changed - every label, order and index the\n" +
       "      user sees comes from it.\n" +
       "      got:  " + JSON.stringify(got) + "\n" +
       "      want: " + JSON.stringify(want_));
}
/*
 * ...and the SAME derivation with the Master FX capabilities off.
 *
 * That is the whole of what makes Master FX a chain editor rather than a second
 * implementation of one: it hands chainEditorComponents a target with no synth
 * and no MIDI FX section, and gets the audio-FX end of this same list. A
 * `caps` argument that stopped being honoured would show up here as the synth
 * reappearing in the master row -- where the diagram would paint its landmark
 * band on a chain that has no synth to landmark.
 */
const mfx = shape(derive(CFG, { hasSynth: false, hasMidiFx: false }));
const wantMfx = [
  { key: "fx1",      label: "FX 1",     kind: "module",   position: 0 },
  { key: "fx2",      label: "FX 2",     kind: "module",   position: 1 },
  { key: "add_fx",   label: "Add FX",   kind: "add",      position: 2 },
  { key: "settings", label: "Settings", kind: "settings", position: 3 },
];
if (JSON.stringify(mfx) !== JSON.stringify(wantMfx)) {
  fail("the Master FX derivation of the same list changed.\n" +
       "      got:  " + JSON.stringify(mfx) + "\n" +
       "      want: " + JSON.stringify(wantMfx));
}
/* And the capabilities are read INDEPENDENTLY -- a single "is this master"
   flag would make these two indistinguishable. */
const noSynthOnly = shape(derive(CFG, { hasSynth: false, hasMidiFx: true }));
if (noSynthOnly.some((c) => c.kind === "synth"))
  fail("hasSynth:false left the synth in the list");
if (!noSynthOnly.some((c) => c.key === "midiFx"))
  fail("hasSynth:false also dropped the MIDI FX section, so the two capabilities " +
       "are one flag wearing two names");

console.log("PASS: chain editor list — derived from chain_model, " + want_.length +
            " positions for a slot chain and " + wantMfx.length + " for Master FX out " +
            "of the SAME derivation, labels and order unchanged");
'
