#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Static checks on the shadow_ui.js side of the component-load hold.
#
# The decision itself is unit-tested in test_component_load_gate.sh. What that
# cannot see is whether shadow_ui.js actually ASKS it — and a gate nobody calls
# is exactly the shape the bug had: every piece of machinery that knows how to
# wait for a slow module was already in the tree, sitting behind an entry point
# that had already decided to show the fallback.
#
# So this pins the wiring: that the entry point routes through the gate rather
# than branching on one read, that the wait is drawn, serviced and escapable on
# BOTH draw paths (main and co-run), and that the reads it makes are raw.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the component-load-hold wiring tests" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cp src/shadow/shadow_ui.js "$TMP/shadow_ui.mjs"
if ! node --check "$TMP/shadow_ui.mjs" 2>"$TMP/err"; then
  echo "FAIL: shadow_ui.js does not parse:"; cat "$TMP/err"; exit 1
fi

node -e '
const fs = require("fs");
const s = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");

let failed = 0;
const fail = (m) => { console.log("FAIL: " + m); failed++; };
const want = (re, m) => { if (!re.test(s)) fail(m); };
const bodyOf = (decl) => {
  const at = s.indexOf(decl);
  if (at < 0) return null;
  const end = s.indexOf("\n}\n", at);
  return s.slice(at, end < 0 ? s.length : end);
};

/* ---- the entry point must not decide from one read -------------------- */
{
  const body = bodyOf("function enterHierarchyEditor(slotIndex, componentKey) {");
  if (body === null) fail("enterHierarchyEditor is gone");
  else {
    if (!/openComponentEditor\(/.test(body))
      fail("enterHierarchyEditor does not go through the load gate");
    /* The original bug, spelled out: one read, then an irreversible branch to
     * the preset-browser fallback. `getComponentHierarchy` collapses null and
     * "" into null, so it can no longer be what the entry decision rests on. */
    if (/getComponentHierarchy\(/.test(body))
      fail("enterHierarchyEditor is back to deciding from a collapsed read");
    if (/enterComponentEditFallback\(/.test(body))
      fail("enterHierarchyEditor falls back directly, bypassing the hold");
  }
}

/* Master FX comes up just as slowly and holds the same modules. */
{
  const body = bodyOf("function enterMasterFxHierarchyEditor(fxSlot) {");
  if (body === null) fail("enterMasterFxHierarchyEditor is gone");
  else if (!/openComponentEditor\(/.test(body))
    fail("Master FX skips the load gate — a slow module blanks its editor too");
}

/* ---- the gate is consulted, and HOLD is honoured ---------------------- */
{
  const body = bodyOf("function openComponentEditor(slotIndex, componentKey, mfxIndex) {");
  if (body === null) fail("openComponentEditor is gone");
  else {
    if (!/decideComponentEntry\(/.test(body)) fail("openComponentEditor does not ask the gate");
    if (!/ENTRY_HOLD/.test(body)) fail("openComponentEditor ignores a HOLD — the wait never happens");
    if (!/ENTRY_ENTER/.test(body)) fail("openComponentEditor never opens anything");
  }
}

/* ---- the reads must be RAW ------------------------------------------- *
 *
 * null (the read did not complete) and "" (served, nothing there) are
 * different answers, and the reader is the last place that can tell them
 * apart. Routing any of the three through a helper that collapses them
 * (getComponentHierarchy, or a `|| ""`) puts the bug straight back.
 */
{
  const body = bodyOf("function componentEntryReader(slotIndex, componentKey, mfxIndex) {");
  if (body === null) fail("componentEntryReader is gone");
  else {
    if (/getComponentHierarchy\(/.test(body))
      fail("componentEntryReader reads through the collapsing helper");
    if (/getMasterFxHierarchy\(/.test(body))
      fail("componentEntryReader reads Master FX through the collapsing helper");
    if (/_module`\)\s*\|\|\s*""/.test(body) || /:module`\)\s*\|\|\s*""/.test(body))
      fail("componentEntryReader defaults a failed module read to \"\" — null is not \"\"");
    for (const k of ["ui_hierarchy", "is_loading"])
      if (!body.includes(k)) fail(`componentEntryReader never reads ${k}`);
  }
}

/* ---- the wait is drawn, serviced and escapable ------------------------ *
 *
 * On BOTH draw paths. The co-run path is a mirror of the main switch and has
 * drifted from it before; a view drawn on one and not the other is a screen
 * that goes blank the moment a tool is co-running — which is the failure this
 * whole change is about.
 */
want(/COMPONENT_LOADING:\s*"comploading"/, "VIEWS.COMPONENT_LOADING is gone");

{
  const draws = s.match(/case VIEWS\.COMPONENT_LOADING:/g) || [];
  if (draws.length < 3)
    fail(`VIEWS.COMPONENT_LOADING is dispatched ${draws.length} time(s); want 3 ` +
         "(main draw, co-run draw, Back)");
}
want(/drawComponentLoading\(\)/, "the wait is never drawn");
want(/function drawComponentLoading\(\)/, "drawComponentLoading is gone");
want(/cancelComponentLoadHold\(\)/, "Back cannot escape the wait");

/* Serviced on both paths, and BEFORE the dispatch — a probe that lands
 * changes the view, and servicing inside the draw case would show the
 * "Loading..." frame once more after the module was already readable. */
{
  const calls = s.match(/serviceComponentLoadHold\(\);/g) || [];
  if (calls.length < 2)
    fail(`serviceComponentLoadHold is called ${calls.length} time(s); want 2 ` +
         "(main tick and co-run dispatch)");
  /* Not inside the case it would be servicing: the probe has to run before the
   * dispatch, or a probe that lands still draws one more "Loading..." frame. */
  const armsAt = [];
  for (let at = s.indexOf("case VIEWS.COMPONENT_LOADING:"); at >= 0;
       at = s.indexOf("case VIEWS.COMPONENT_LOADING:", at + 1)) {
    armsAt.push(at);
    const next = s.indexOf("case VIEWS.", at + 11);
    if (/serviceComponentLoadHold\(/.test(s.slice(at, next < 0 ? at + 400 : next)))
      fail("the probe runs inside the draw dispatch rather than before it");
  }

  /* Each of the two DRAW dispatches (the first arm is the Back handler) is
   * preceded by a probe, with no other dispatch in between. */
  const callAt = [];
  for (let at = s.indexOf("serviceComponentLoadHold();"); at >= 0;
       at = s.indexOf("serviceComponentLoadHold();", at + 1)) callAt.push(at);
  for (const arm of armsAt.slice(1)) {
    const before = callAt.filter((c) => c < arm);
    if (!before.length) { fail("a draw dispatch of the wait has no probe before it"); continue; }
    const nearest = before[before.length - 1];
    /* Between the probe and the case there must be no OTHER draw dispatch of
     * the wait — otherwise one probe would be credited to both switches. */
    if (armsAt.some((a) => a > nearest && a < arm))
      fail("one probe is being credited to two draw dispatches");
  }
}

/* ---- the probe never stops ------------------------------------------- *
 *
 * A bounded budget that ends in the fallback would restore the blank editor
 * with extra steps. Holding uses the gate published cadence, which slows but
 * does not stop.
 */
{
  const body = bodyOf("function serviceComponentLoadHold() {");
  if (body === null) fail("serviceComponentLoadHold is gone");
  else {
    if (!/holdProbeIntervalTicks\(/.test(body))
      fail("the hold does not use the published probe cadence");
    if (!/openComponentEditor\(/.test(body))
      fail("the hold never re-asks the gate, so it can never open");
  }
}

if (failed) { console.log(`FAILED: ${failed} check(s)`); process.exit(1); }
console.log("PASS: component load hold — entry gated, reads raw, wait drawn/serviced/escapable on both paths");
'
