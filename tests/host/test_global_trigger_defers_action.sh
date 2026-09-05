#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A DOOR ON THE SETTINGS PAGE MUST NOT ACT FROM INSIDE THE WRITE.
#
# Global Settings' Web Manager and Help rows are write-only params (momentaries)
# rather than a menu page, which is what lets three rows share one screen. The
# cost is that their write happens INSIDE the page controller: onClick ->
# fireTrigger -> setParam, all within one applyInput. Both actions navigate —
# they leave the knob grid and open another view — and running that from setParam
# tears the controller down while applyInput is still executing on it.
#
# That shipped. Reported from the device: Back came out on the wrong page, and
# every setting on the page read zero.
#
# THE REASON THIS FILE EXISTS RATHER THAN A GREP: nothing about the broken
# version looks wrong. The call was present, correct, and in a plausible place;
# only its ORDERING relative to the controller was wrong, and a source-level pin
# cannot see ordering. So this drives the real controller through a real click
# and asserts what happened and WHEN.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const G = await import(R + "/src/shadow/shadow_ui_global_grid.mjs");
const { createController, LAYOUT_LIST } =
    await import(R + "/src/shared/param_pages/page_controller.mjs");

/* The section the doors live on, derived rather than spelled: the page name is
 * the section LABEL, and hardcoding it here would stop matching the day it is
 * reworded — silently, because the walk below would just never find the row. */
const SYSTEM_PAGE = (G.GLOBAL_SECTIONS.find((s) => s.id === "system") || {}).label;
if (!SYSTEM_PAGE) fail("no system section");

function harness() {
    const log = [];
    const io = G.createGlobalGridIo({
        readParam: () => "0",
        writeParam: (k, v) => log.push("WRITE " + k + "=" + v),
        runAction: (a) => log.push("RUNACTION " + a),
    });
    const ctrl = createController({
        getParam: io.getParam, setParam: io.setParam,
        isModulated: io.isModulated,
        /* Deliberately wired the way the host wires it. If the controller ever
         * calls runAction for a trigger by itself, that shows up here. */
        runAction: (a) => log.push("CONTROLLER-RUNACTION " + a),
        announce: (m) => log.push("SAY " + m),
    });
    ctrl.load({ prefix: "", paginate: false });
    ctrl.setLayout(LAYOUT_LIST);
    for (let t = 0; t < 24; t++) ctrl.tick();
    /* Land on the System page by NAME, the way the host restores one. */
    ctrl.restorePage(SYSTEM_PAGE, {});
    for (let t = 0; t < 12; t++) ctrl.tick();
    return { io, ctrl, log };
}

/* ---- 1. the click queues; it does NOT act ------------------------------- */
for (const [rowName, wantAction] of [["Web Manager", "connect"], ["Help", "help"]]) {
    const { io, ctrl, log } = harness();
    if (ctrl.pageLabel() !== SYSTEM_PAGE) {
        fail("could not land on " + SYSTEM_PAGE + ", got " + ctrl.pageLabel());
        continue;
    }
    ctrl.enterMenu();
    /* Walk to the row by what it ANNOUNCES, so the test follows the cursor the
     * user follows rather than an index that a reordered page would invalidate. */
    let found = false;
    for (let i = 0; i < 8; i++) {
        const said = log.filter((l) => l.startsWith("SAY ")).pop() || "";
        if (said.slice(4).startsWith(rowName + ",")) { found = true; break; }
        ctrl.onJog(1);
    }
    if (!found) { fail("never reached the " + rowName + " row"); continue; }

    const before = log.length;
    ctrl.onClick(0);
    const during = log.slice(before);

    /*
     * NOTHING RAN. This is the whole assertion: the click may write, announce
     * or do nothing at all, but it must not reach an action while the
     * controller is mid-input.
     */
    const acted = during.filter((l) => l.indexOf("RUNACTION") >= 0);
    if (acted.length) {
        fail(rowName + ": the action ran from inside the click (" + acted.join(", ") +
             ") -- this tears the controller down while applyInput is still on it");
    }
    /* And it must not have written the key through to a backend either: there
     * is no backend, and a write that fell through would set a param named
     * `connect` on something. */
    const wrote = during.filter((l) => l.startsWith("WRITE "));
    if (wrote.length) fail(rowName + ": wrote through to a backend: " + wrote.join(", "));

    /* ---- 2. ...but the action IS queued, exactly once ------------------- */
    const queued = io.takePendingAction();
    if (queued !== wantAction) {
        fail(rowName + ": queued " + JSON.stringify(queued) + ", want " + JSON.stringify(wantAction));
    }
    /* Taken, not read: a second drain must be empty or the action fires again
     * on the next input the grid sees. */
    const again = io.takePendingAction();
    if (again !== null) fail(rowName + ": the queue still holds " + JSON.stringify(again) + " after a take");

    /* ---- 3. the controller SURVIVED the click --------------------------- */
    if (ctrl.pageLabel() !== SYSTEM_PAGE) {
        fail(rowName + ": the page moved to " + ctrl.pageLabel() + " -- the click must not navigate");
    }
    for (let t = 0; t < 6; t++) ctrl.tick();
    const vm = ctrl.describePage ? ctrl.describePage() : null;
    if (!vm) fail(rowName + ": the controller cannot describe its page after the click");
}

/* ---- 4. an ordinary setting still writes straight through ---------------
 *
 * Without this the queue could swallow EVERY write and sections 1-3 would all
 * still pass -- a settings screen that quietly stopped saving anything.
 */
{
  const { io, log } = harness();
  io.setParam("analytics_enabled", "1");
  const wrote = log.filter((l) => l.startsWith("WRITE analytics_enabled"));
  if (!wrote.length) fail("analytics_enabled did not write through: " + JSON.stringify(log.slice(-4)));
  if (io.takePendingAction() !== null) fail("an ordinary setting queued an action");
}

/* ---- 5. the host drains the queue where it drains a menu action ---------
 *
 * The two halves live in different files and only work together. This is the
 * source half -- it cannot prove the ordering (that is sections 1-3), but it
 * can prove the drain exists, is called on the input path, and runs BEFORE the
 * `if (!todo)` early return that a trigger takes.
 */
{
  const FS = await import("node:fs");
  const src = FS.readFileSync(R + "/src/shadow/shadow_ui_param_pages.mjs", "utf8");
  const drain = src.indexOf("takePendingAction");
  const apply = src.indexOf("applyInput(controller, intent");
  const early = src.indexOf("if (!todo) return true;");
  if (drain < 0) fail("shadow_ui_param_pages.mjs never drains takePendingAction");
  else if (!(apply < drain && drain < early)) {
    fail("the drain must sit between applyInput and the `if (!todo)` early return " +
         "(applyInput@" + apply + ", drain@" + drain + ", early-out@" + early + ")");
  }
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: settings triggers defer — a click on Web Manager or Help queues its action " +
            "instead of running it, the controller survives the click on the same page, the " +
            "queue is taken once, ordinary settings still write through, and the host drains " +
            "the queue between applyInput and its early return");
'
