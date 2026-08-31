#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# One reconciler, four handlers.
#
# runSlotActionFromGrid / runMasterFxActionFromGrid / runGlobalActionFromGrid /
# runComponentActionFromGrid each fire a menu action from the knob grid and
# then have to decide whether the action put something else on screen -- an
# overlay flag, a help stack, or a whole new view -- so the grid can hand off
# and later reconcile back to itself. Before this change that question was
# copy-pasted three times (Global Settings' own comment named the risk: "a
# test on the key would be right today and silently wrong for the fourth
# action") and the fourth handler, added for component User Presets / Module
# actions, had no such question at all.
#
# This pins: ONE shared predicate exists and every handler calls it; no
# handler decides by testing the action KEY; each handler still performs its
# OWN follow-on (the differences are real -- slot switches to CHAIN_SETTINGS,
# Master FX is addressed at IPC slot 0 by convention, Global also treats a
# pushed help stack as "something opened", and the component handler's
# convergence point is VIEWS.CHAIN_EDIT rather than a view of its own); and
# that all four maybeReturnTo* reconcilers are still wired into the poll site
# that runs before the draw switch, unchanged in shape.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node -e '
const fs = require("fs");
const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (m) => { console.error("ok: " + m); };

/* Extract one top-level function body by name -- same technique as
   test_chain_edit_read_budget.sh: the file cannot be imported (a device UI
   module full of host globals), so a lifted function is run with its
   dependencies handed in as parameters, and a source-text search finds the
   handlers that only get pinned structurally. */
function findBody(name) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) return null;
  const end = src.indexOf("\n}\n", at);
  if (end < 0) return null;
  return src.slice(at, end + 2);
}
function lift(name, deps) {
  const body = findBody(name);
  if (!body) { fail(name + " is gone"); return null; }
  return new Function(...deps, body + "\nreturn " + name + ";");
}

/* ---- 1. The predicate exists exactly once, and behaves like an OR. ------- */

const predicateName = "gridActionOpenedSomething";
const predicateCount = src.split("function " + predicateName + "(").length - 1;
if (predicateCount === 1) ok("gridActionOpenedSomething defined exactly once");
else fail("expected exactly one gridActionOpenedSomething, found " + predicateCount);

const liftedPredicate = lift(predicateName, []);
const gridActionOpenedSomething = liftedPredicate ? liftedPredicate() : null;
if (gridActionOpenedSomething) {
  /* Proves the lifted code actually RAN, not just parsed -- a spy would be
     overkill for a pure function, so the assertions themselves are the
     "it ran" evidence: a stub that silently returned undefined either way
     would fail both of these. */
  if (gridActionOpenedSomething(false, false, false) === false) {
    ok("all-false -> false (ran, and did not just return truthy)");
  } else {
    fail("gridActionOpenedSomething(false, false, false) should be false");
  }
  if (gridActionOpenedSomething(false, true, false) === true) {
    ok("one true among several -> true");
  } else {
    fail("gridActionOpenedSomething(false, true, false) should be true");
  }
  if (gridActionOpenedSomething(false) === false && gridActionOpenedSomething(true) === true) {
    ok("single-condition callers (Global Settings view-changed test) still work");
  } else {
    fail("gridActionOpenedSomething did not behave as an OR over its arguments");
  }
}

/* ---- 2. All four handlers call the shared predicate. --------------------- */

const HANDLERS = [
  "runSlotActionFromGrid",
  "runMasterFxActionFromGrid",
  "runGlobalActionFromGrid",
  "runComponentActionFromGrid",
];
for (const name of HANDLERS) {
  const body = findBody(name);
  if (!body) { fail(name + " is gone"); continue; }
  if (body.includes(predicateName + "(")) {
    ok(name + " calls " + predicateName);
  } else {
    fail(name + " does not call " + predicateName);
  }
}

/* ---- 3. No handler decides by testing the action KEY. -------------------- */

/* The switch/case dispatch that RUNS the right code for "up_load" vs
   "up_save" is not the thing being pinned against -- that is an unavoidable
   dispatch, not the reconcile decision. What must never come back is an
   equality test on the action (or the settings key) used to decide whether
   something opened, e.g. `if (action === "save")` -- the exact shape the
   Global Settings handler comment warns is "right today and silently wrong
   for the fourth action". */
const KEY_TEST_PATTERNS = [
  /\baction\s*===/,
  /===\s*action\b/,
  /\bkey\s*===\s*"/,
  /"\s*===\s*key\b/,
];
for (const name of HANDLERS) {
  const body = findBody(name);
  if (!body) continue;
  const hit = KEY_TEST_PATTERNS.find((re) => re.test(body));
  if (!hit) {
    ok(name + " does not test the action/key by equality");
  } else {
    fail(name + " decides by testing the action key (" + hit + ") -- exactly the fragile pattern this refactor removes");
  }
}

/* ---- 4. Each handler still performs its OWN follow-on. ------------------- */

const FOLLOWONS = {
  runSlotActionFromGrid: ["suppressSlotGridOnce", "slotModalFromGrid", "VIEWS.CHAIN_SETTINGS"],
  runMasterFxActionFromGrid: ["suppressMasterGridOnce", "masterModalFromGrid", "VIEWS.MASTER_FX"],
  runGlobalActionFromGrid: ["globalModalFromGrid", "helpNavStack", "VIEWS.GLOBAL_SETTINGS"],
  runComponentActionFromGrid: ["componentModalFromGrid", "componentGridReturnSlot", "componentGridReturnKey"],
};
for (const [name, needles] of Object.entries(FOLLOWONS)) {
  const body = findBody(name);
  if (!body) continue;
  const missing = needles.filter((n) => !body.includes(n));
  if (missing.length === 0) {
    ok(name + " still performs its own follow-on (" + needles.join(", ") + ")");
  } else {
    fail(name + " lost its own follow-on: missing " + missing.join(", "));
  }
}

/* Master FX is addressed at IPC slot 0 by CONVENTION, not instrument slot 1 --
   getting this wrong would save instrument slot 1 patch from the master bus.
   masterGridIoFor is the function that wires that convention in; pin that it
   still reads/writes through getSlotParam/setSlotParam(0, ...). */
{
  const at = src.indexOf("function masterGridIoFor(");
  const end = at >= 0 ? src.indexOf("\nfunction ", at + 1) : -1;
  const body = at >= 0 && end > at ? src.slice(at, end) : "";
  if (body.includes("getSlotParam(0,") && body.includes("setSlotParam(0,")) {
    ok("masterGridIoFor still addresses IPC slot 0 by convention");
  } else {
    fail("masterGridIoFor no longer visibly addresses IPC slot 0 -- Master FX save could hit the wrong slot");
  }
}

/* ---- 5. The reconcile-dont-hook shape survives, for all four. ------------ */

const RECONCILERS = [
  ["maybeReturnToSlotGrid", "VIEWS.CHAIN_SETTINGS", "slotModalFromGrid"],
  ["maybeReturnToMasterGrid", "VIEWS.MASTER_FX", "masterModalFromGrid"],
  ["maybeReturnToGlobalGrid", "VIEWS.GLOBAL_SETTINGS", "globalModalFromGrid"],
  ["maybeReturnToComponentGrid", "VIEWS.CHAIN_EDIT", "componentModalFromGrid"],
  /* The fifth: "Module Help" off the Module page of a component. It shares
     GLOBAL_SETTINGS with maybeReturnToGlobalGrid (the help viewer has no view
     of its own) and is told apart by its own pending return, not by a
     *ModalFromGrid flag. */
  ["maybeReturnToComponentHelp", "VIEWS.GLOBAL_SETTINGS", "componentHelpReturnSlot"],
];
for (const [fn, gateView, flag] of RECONCILERS) {
  const body = findBody(fn);
  if (!body) { fail(fn + " is gone"); continue; }
  if (!body.includes(flag)) fail(fn + " no longer gates on " + flag);

  /* Wired into the poll site: `if (view === <gateView>) <fn>();`, run before
     the draw switch. A structural grep, not a behavioural one, because the
     poll site sits inside the giant tick() closure with dozens of host
     globals -- lifting it would mean faking most of shadow_ui.js. */
  const wireRe = new RegExp(
    "if\\s*\\(\\s*view\\s*===\\s*" + gateView.replace(".", "\\.") + "\\s*\\)\\s*" + fn + "\\(\\)"
  );
  if (wireRe.test(src)) {
    ok(fn + " wired at the poll site, gated on " + gateView);
  } else {
    fail(fn + " is not wired into the poll site as `if (view === " + gateView + ") " + fn + "()`");
  }
}

/* All the poll-site lines must appear together, in the same block, so a
   later edit cannot silently drop one while touching the others. */
{
  const pollAt = src.indexOf("maybeReturnToSlotGrid();");
  const pollEnd = src.indexOf("maybeReturnToComponentGrid();");
  if (pollAt >= 0 && pollEnd > pollAt && pollEnd - pollAt < 2000) {
    ok("all poll-site calls sit together in one block");
  } else {
    fail("the poll-site calls are not co-located -- expected all within ~2000 chars of each other");
  }
}

/* ---- 6. The two leftover applyComponentSelectionConfirmed log strings ---- */
/*         were renamed to applyChainComponentPick, and the comment noting   */
/*         the slot+key re-derivation is agreement, not redundancy.         */

if (!src.includes("applyComponentSelection:")) {
  ok("no host_log string still says applyComponentSelection:");
} else {
  fail("a host_log string still says applyComponentSelection: (should be applyChainComponentPick:)");
}
if (src.includes("applyChainComponentPick: slot=") && src.includes("applyChainComponentPick: setSlotParam returned")) {
  ok("both host_log strings renamed to applyChainComponentPick:");
} else {
  fail("expected both host_log strings under applyChainComponentPick:");
}
if (src.includes("Re-derived from slot+key") && /expected\s*\n?\s*\*?\s*to\s*\n?\s*\*?\s*always agree/.test(src)) {
  ok("comment on the slot+key re-derivation agreement is present");
} else {
  fail("missing the one-line comment noting the two lookups are expected to agree");
}

/* ---- 7. Component handler`s follow-on is genuinely new, not a copy. ----- */
/* It must NOT reuse suppressSlotGridOnce/suppressMasterGridOnce -- there is */
/* no list to suppress, only a real navigation to come back from.          */
{
  const body = findBody("runComponentActionFromGrid") || "";
  if (!body.includes("suppressSlotGridOnce") && !body.includes("suppressMasterGridOnce")) {
    ok("component handler does not borrow another handlers suppress flag");
  } else {
    fail("component handler reused a suppress flag that belongs to a different screen");
  }
}

/* ---- 8. BEHAVIOUR: maybeReturnToComponentGrid must prove the arrival, ---- */
/*         not just trust the flag.                                        */
/*                                                                          */
/* CHAIN_EDIT is reachable from places that have nothing to do with a fired */
/* grid action (Shift+Vol+TrackN jumps, long-press, ...) and run every tick */
/* regardless of view -- so "the flag is set and view is CHAIN_EDIT" is not */
/* proof this arrival is the flow that raised the flag. A JUMP_TO_SLOT(3)   */
/* while the flag was raised for slot 1 must NOT yank the user out of the   */
/* slot-3 chain editor they deliberately opened. Driven behaviourally       */
/* (lifted and RUN, not grepped) because a structural pin on the flag name  */
/* alone would pass with the body having zero safety -- which is exactly    */
/* what shipped before this was caught in review.                          */
{
  const body = findBody("maybeReturnToComponentGrid");
  if (!body) {
    fail("maybeReturnToComponentGrid is gone");
  } else {
    const run = (state) => {
      const log = { entered: 0 };
      const s = Object.assign({
        componentModalFromGrid: true,
        componentGridReturnSlot: 1,
        componentGridReturnKey: "synth",
        /* the door-open disposition -- see the restorePage note */
        componentGridReturnEnter: true,
        selectedSlot: 1,
        needsRedraw: false,
        /* The position still holds a module unless a case says otherwise.
           Remove Module is one of the actions that hands off, so "the slot
           matches" is not enough on its own -- see 8e. */
        chainConfigs: [null, { synth: { module: "obxd" } }, null, null],
      }, state);
      const textEntryActive = !!state.textEntryActive;
      const isTextEntryActive = () => textEntryActive;
      const getChainComponentModule = (cfg, key) => (cfg && cfg[key]) || null;
      const getComponentParamPrefix = (k) => k;
      const componentParamPagesIo = () => ({ marker: "io" });
      const paramPagesChromeFor = () => ({ marker: "chrome" });
      const enterParamPages = (...args) => { log.entered++; log.args = args; };
      const patched = body
        .replace(/\bcomponentModalFromGrid\b/g, "s.componentModalFromGrid")
        .replace(/\bcomponentGridReturnSlot\b/g, "s.componentGridReturnSlot")
        .replace(/\bcomponentGridReturnKey\b/g, "s.componentGridReturnKey")
        .replace(/\bcomponentGridReturnEnter\b/g, "s.componentGridReturnEnter")
        .replace(/\bselectedSlot\b/g, "s.selectedSlot")
        .replace(/\bchainConfigs\b/g, "s.chainConfigs")
        .replace(/\bneedsRedraw\b/g, "s.needsRedraw");
      const fn = new Function(
        "s", "isTextEntryActive", "getComponentParamPrefix",
        "componentParamPagesIo", "paramPagesChromeFor", "enterParamPages",
        "getChainComponentModule",
        patched + "\nreturn maybeReturnToComponentGrid;"
      )(s, isTextEntryActive, getComponentParamPrefix, componentParamPagesIo, paramPagesChromeFor, enterParamPages,
        getChainComponentModule);
      return { fired: fn(), log, s };
    };

    /* 8a. Legitimate convergence: user backed all the way out to CHAIN_EDIT
       on the SAME slot the hand-off was raised for. Must fire, must re-enter
       the grid, must drop the flag. */
    {
      const { fired, log, s } = run({ selectedSlot: 1 });
      if (fired === true && log.entered === 1 && s.componentModalFromGrid === false) {
        ok("fires and re-enters the grid on the slot it was raised for");
      } else {
        fail("did not fire (or did not re-enter/clear) for the legitimate same-slot arrival: " +
             JSON.stringify({ fired, entered: log.entered, flag: s.componentModalFromGrid }));
      }
    }

    /* 8b. The reported repro: up_load raised the flag for slot 1, then
       Shift+Vol+Track3 (JUMP_TO_SLOT) reassigns selectedSlot to 3 and lands on
       CHAIN_EDIT before the user backed out. Must NOT re-enter the grid over
       the slot-3 chain editor the user deliberately opened, and must drop the
       now-stale flag so it cannot fire on some later, unrelated arrival. */
    {
      const { fired, log, s } = run({ selectedSlot: 3 });
      if (fired === false && log.entered === 0 && s.componentModalFromGrid === false) {
        ok("does NOT fire on a mismatched-slot arrival, and drops the stale flag (the JUMP_TO_SLOT repro)");
      } else {
        fail("fired (or left the flag set) on a mismatched-slot arrival -- this is the reported [Critical] bug: " +
             JSON.stringify({ fired, entered: log.entered, flag: s.componentModalFromGrid }));
      }
    }

    /* 8e. REPORTED FROM HARDWARE. Remove Module is one of the actions that
       hands off, and it ends in setView(CHAIN_EDIT) on the SAME slot -- so the
       slot match passes and the reconciler would re-enter the grid for the
       component that was just deleted. A contract read with nobody to answer
       it, which the device draws as a permanent "Loading...". The position
       must still hold a module before we go back to it. */
    {
      const { fired, log, s } = run({
        selectedSlot: 1,
        chainConfigs: [null, { synth: null }, null, null],
      });
      if (fired === false && log.entered === 0 && s.componentModalFromGrid === false) {
        ok("does NOT re-enter the grid for a component that was just REMOVED (the Loading... repro)");
      } else {
        fail("re-entered the grid for a removed component -- this is the hardware-reported " +
             "Loading... screen: " +
             JSON.stringify({ fired, entered: log.entered, flag: s.componentModalFromGrid }));
      }
    }

    /* 8f. ...but a SWAP leaves a different module in the same position, and
       that IS something to go back to. The guard must key on whether a module
       is present, not on whether the module changed. */
    {
      const { fired, log } = run({
        selectedSlot: 1,
        chainConfigs: [null, { synth: { module: "braids" } }, null, null],
      });
      if (fired === true && log.entered === 1) {
        ok("still fires after a SWAP, where the position holds a different module");
      } else {
        fail("failed to return to the grid after a swap: " +
             JSON.stringify({ fired, entered: log.entered }));
      }
    }

    /* 8c. No flag raised at all: must be an inert no-op, every tick. */
    {
      const { fired, log } = run({ componentModalFromGrid: false, selectedSlot: 1 });
      if (fired === false && log.entered === 0) {
        ok("is a no-op when the flag was never raised");
      } else {
        fail("fired with no flag set");
      }
    }

    /* 8d. Same slot, but a text-entry overlay is up (mirrors every sibling
       reconciler): must not steal the keyboard out from under the user. */
    {
      const { fired, log, s } = run({ selectedSlot: 1, textEntryActive: true });
      if (fired === false && log.entered === 0 && s.componentModalFromGrid === true) {
        ok("does not fire over an active text entry, and leaves the flag raised for later");
      } else {
        fail("fired (or dropped the flag) while a text entry was active");
      }
    }
  }
}

if (failures > 0) {
  console.error(failures + " failure(s)");
  process.exit(1);
}
console.error("all checks passed");
'

echo "PASS"
