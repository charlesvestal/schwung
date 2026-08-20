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
want(/let paramViewGlobal = 0;/, "Param View must default to 0 (the list) — this ships as an opt-in preview");
want(/globalThis\.param_view_get_mode/, "the view module reads the setting through a global that is not defined");
want(/function saveParamViewConfig/, "the setting does not persist");
want(/loadParamViewConfig\(\);/, "the setting is never loaded at init");

/* ---- the hand-off cannot loop ---------------------------------------- */
want(/suppressParamPagesOnce = true;\s*\n\s*enterHierarchyEditor\(/,
     "the grid hands off to the list without setting the anti-loop guard");
want(/paramPagesEnabled\(\) && !suppressParamPagesOnce/,
     "list entry ignores the anti-loop guard — handing off would bounce back into the grid forever");
want(/suppressParamPagesOnce = false;/, "the guard is never cleared, so the grid would stay disabled");

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

console.log("PASS: shadow wiring — parses, view dispatched/ticked/fed, setting defaults to List, " +
            "hand-off cannot loop, screen reader keeps the list, device keys use the prefix, " +
            "LFO targets resolve to names");
'
