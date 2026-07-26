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
want(/case VIEWS\.PARAM_PAGES:\s+if \(!drawParamPages\(\)\)/, "the view is never drawn");
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
if (/PAGE_PRESET|PAGE_ITEMS|PAGE_MODES/.test(v.replace(/PAGE_KNOBS/g, ""))) {
  fail("the view module is drawing page kinds it should hand to the list");
}

console.log("PASS: shadow wiring — parses, view dispatched/ticked/fed, setting defaults to List, " +
            "hand-off cannot loop, screen reader keeps the list");
'
