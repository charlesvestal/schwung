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
