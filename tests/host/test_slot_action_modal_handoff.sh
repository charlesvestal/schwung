#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A slot action that opens a modal must hand the grid back to the LIST view.
#
# Save / Save As / Delete do not act immediately -- they set showingNamePreview
# or confirmingOverwrite / confirmingDelete and wait for a confirmation. Both
# the drawing of those and the jog/click that answer them live under
# `case VIEWS.CHAIN_SETTINGS`, the list view. Once slot settings started opening
# as the knob grid, the flag flipped in a view that could neither draw nor
# finish it.
#
# Reported from hardware as "when I choose save, simply nothing happens", with
# no jog announcement afterwards -- the tell that the action HAD run and landed
# somewhere with no way out. Every unit test passed: the code did exactly what
# it was asked, into a void.
#
# The function is LIFTED from source and RUN, so this tests the behaviour rather
# than the presence of some line. The lift declares the module-level state the
# function closes over as locals and reads them back out.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi

node -e '
const fs = require("fs");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const at = src.indexOf("function runSlotActionFromGrid(slot, key) {");
if (at < 0) {
  fail("runSlotActionFromGrid is gone - the grid no longer hands modal actions to the list");
  process.exit(1);
}
const end = src.indexOf("\n}\n", at);
const body = src.slice(at, end + 2);

/*
 * Build the function with its dependencies as parameters. Each run reports what
 * the action did and what the hand-off did, so the two are checked separately.
 */
function run({ setsFlag }) {
  const log = { exited: 0, view: null, suppressed: false, redraw: false, ran: null };
  let showingNamePreview = false, confirmingOverwrite = false, confirmingDelete = false;
  let suppressSlotGridOnce = false, needsRedraw = false;
  const VIEWS = { CHAIN_SETTINGS: "chainsettings" };
  const runChainSettingAction = (slot, key) => {
    log.ran = [slot, key];
    if (setsFlag === "preview")   showingNamePreview = true;
    if (setsFlag === "overwrite") confirmingOverwrite = true;
    if (setsFlag === "delete")    confirmingDelete = true;
  };
  const exitParamPages = () => { log.exited++; };
  const setView = (v) => { log.view = v; };

  /* eslint-disable no-new-func */
  const fn = new Function(
    "runChainSettingAction", "exitParamPages", "setView", "VIEWS", "state",
    body.replace(/showingNamePreview/g, "state.showingNamePreview")
        .replace(/confirmingOverwrite/g, "state.confirmingOverwrite")
        .replace(/confirmingDelete/g, "state.confirmingDelete")
        .replace(/suppressSlotGridOnce/g, "state.suppressSlotGridOnce")
        .replace(/needsRedraw/g, "state.needsRedraw") +
    "\nreturn runSlotActionFromGrid;");
  const state = {
    showingNamePreview: false, confirmingOverwrite: false, confirmingDelete: false,
    suppressSlotGridOnce: false, needsRedraw: false,
  };
  const wrapped = (slot, key) => {
    const inner = (s, k) => {
      log.ran = [s, k];
      if (setsFlag === "preview")   state.showingNamePreview = true;
      if (setsFlag === "overwrite") state.confirmingOverwrite = true;
      if (setsFlag === "delete")    state.confirmingDelete = true;
    };
    return fn(inner, exitParamPages, setView, VIEWS, state)(slot, key);
  };
  const handed = wrapped(2, "save");
  return { handed, log, state };
}

/* ---- 1. every modal flag hands off ------------------------------------ */
for (const flag of ["preview", "overwrite", "delete"]) {
  const { handed, log, state } = run({ setsFlag: flag });
  if (!log.ran) fail(flag + ": the action itself never ran");
  if (!handed) fail(flag + ": opened a modal but did NOT hand off - it would be invisible on the grid");
  if (log.exited !== 1) fail(flag + ": exitParamPages called " + log.exited + " times, want 1");
  if (log.view !== "chainsettings") fail(flag + ": handed to view " + log.view + ", want CHAIN_SETTINGS");
  if (!state.suppressSlotGridOnce) {
    fail(flag + ": did not suppress re-entering the grid - the list must stay the " +
         "list while the modal is up, or the confirmation is dropped again");
  }
  if (!state.needsRedraw) fail(flag + ": no redraw requested, so the modal would not appear until something else drew");
}

/* ---- 2. a NON-modal action must NOT hand off --------------------------- *
 *
 * Knob Mapping and the LFO doors open their own views. Bouncing those to the
 * list would be a different bug with the same shape. */
{
  const { handed, log, state } = run({ setsFlag: null });
  if (handed) fail("a non-modal action handed off anyway");
  if (log.exited !== 0) fail("a non-modal action tore down the grid");
  if (log.view !== null) fail("a non-modal action changed the view to " + log.view);
  if (state.suppressSlotGridOnce) fail("a non-modal action suppressed the grid");
}

if (failures) process.exit(1);
console.log("PASS: slot action modal hand-off - all three confirm flows reach the " +
            "view that can draw and answer them; non-modal actions are left alone");
'

# And the return trip: once the modal is done, go back to the GRID.
#
# Reported from hardware: "i was able to save but then was taken back to the
# menu not the page". You opened the grid, so that is where you should still be.
#
# This RECONCILES rather than firing at the end of each flow, because the modal
# finishes in several ways - confirm, decline, Back, and for Save a decline that
# returns to the name preview rather than exiting. Hooking each one means being
# wrong about exactly one of them.
node -e '
const fs = require("fs");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const at = src.indexOf("function maybeReturnToSlotGrid() {");
if (at < 0) {
  fail("maybeReturnToSlotGrid is gone - a modal opened from the grid would strand the user in the list");
  process.exit(1);
}
const body = src.slice(at, src.indexOf("\n}\n", at) + 2);

/* It must be CALLED from the tick, not merely defined. A reconcile nobody runs
 * is the same bug wearing a fix. */
if (!/if \(view === VIEWS\.CHAIN_SETTINGS\) maybeReturnToSlotGrid\(\);/.test(src)) {
  fail("maybeReturnToSlotGrid is never called from the tick");
}
const tickAt = src.indexOf("globalThis.tick = function()");
const callAt = src.indexOf("if (view === VIEWS.CHAIN_SETTINGS) maybeReturnToSlotGrid();");
if (tickAt >= 0 && callAt >= 0 && callAt < tickAt) {
  fail("the reconcile call is OUTSIDE globalThis.tick - shadow_ui.js has several " +
       "view switches and only the one inside tick actually runs each frame");
}

function run(state) {
  const log = { entered: null };
  const s = Object.assign({
    slotModalFromGrid: false, showingNamePreview: false,
    confirmingOverwrite: false, confirmingDelete: false,
    suppressSlotGridOnce: false, selectedSlot: 2,
  }, state);
  const fn = new Function("s", "enterChainSettings",
    body.replace(/slotModalFromGrid/g, "s.slotModalFromGrid")
        .replace(/showingNamePreview/g, "s.showingNamePreview")
        .replace(/confirmingOverwrite/g, "s.confirmingOverwrite")
        .replace(/confirmingDelete/g, "s.confirmingDelete")
        .replace(/suppressSlotGridOnce/g, "s.suppressSlotGridOnce")
        .replace(/selectedSlot/g, "s.selectedSlot") +
    "\nreturn maybeReturnToSlotGrid;")(s, (slot) => { log.entered = slot; });
  return { went: fn(), s, log };
}

/* 1. Modal finished and it came from the grid: go back, to the RIGHT slot. */
{
  const { went, s, log } = run({ slotModalFromGrid: true, suppressSlotGridOnce: true });
  if (!went) fail("the modal finished but the grid was not restored");
  if (log.entered !== 2) fail("returned to slot " + log.entered + ", want the slot the modal was for (2)");
  if (s.suppressSlotGridOnce) {
    fail("the suppression was not consumed - enterChainSettings would spend it and " +
         "hand back the LIST again, so the user still never sees the grid");
  }
  if (s.slotModalFromGrid) fail("the flag was not cleared - it would bounce back on every later visit to the list");
}

/* 2. A modal still open must NOT be yanked away mid-confirmation. */
for (const f of ["showingNamePreview", "confirmingOverwrite", "confirmingDelete"]) {
  const st = { slotModalFromGrid: true }; st[f] = true;
  const { went, log } = run(st);
  if (went || log.entered !== null) fail("returned to the grid while " + f + " was still up");
}

/* 3. Never came from the grid (Param View = List): leave the user alone. */
{
  const { went, log } = run({ slotModalFromGrid: false });
  if (went || log.entered !== null) fail("hijacked the list view for a user who opened the list");
}

if (failures) process.exit(1);
console.log("PASS: slot modal return - the grid comes back once the modal is done, " +
            "not before, and not for list-view users");
'
