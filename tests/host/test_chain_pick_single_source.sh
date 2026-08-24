#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# One function applies a chain-component pick -- so Remove Module (Task 6, the
# Module page) can BE the None row of the picker, instead of a second copy of
# it.
#
# The sequence a pick triggers is not one write: it updates the in-memory
# config, invalidates the cache, tracks slotUserCleared for the autosave
# boot-glitch guard, drops LFO target labels, resolves the paramKey, runs the
# feedback gate (for a non-empty pick only), and finally commits through
# applyComponentSelectionConfirmed -- which itself sends the SHAPE verb before
# the module write. A second implementation of Remove would only need to get
# ONE of those wrong to silently misbehave, so this file pins that there is
# exactly one function that does all of it, and that picked="" really does
# reach the removal branch rather than the feedback-gate branch.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* Lift a top-level function out of shadow_ui.js and hand it its dependencies
   as parameters. The file cannot be imported -- it is a device UI module full
   of host globals -- but a function that closes over a few named things can be
   RUN, which is the difference between pinning the source text and pinning
   the behaviour. */
function lift(name, deps) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return { fn: new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";"),
           body: src.slice(at, end + 2), start: at, end: end + 2 };
}

// --------------------------------------------------------------------------
// 1. STRUCTURAL: one function holds the whole sequence.
// --------------------------------------------------------------------------

const defCount = (src.match(/function applyChainComponentPick\(/g) || []).length;
if (defCount !== 1) fail("applyChainComponentPick must be defined exactly once, found " + defCount);

const lifted = lift("applyChainComponentPick",
  ["chainConfigs", "createEmptyChainConfig", "withPendingChainInsert",
   "applyPickerChoiceToChain", "invalidateChainConfig", "slotUserCleared",
   "chainHasAnyModule", "resetLfoTargetLabels", "chainComponentParamKey",
   "host_get_module_metadata", "host_log", "maybeConfirmForModule",
   "loadChainConfigFromSlot", "setView", "VIEWS", "needsRedraw",
   "applyComponentSelectionConfirmed", "slotChainComponentIndex",
   "slotChainComponents"]);

if (lifted) {
  const mustContain = [
    "applyPickerChoiceToChain", "withPendingChainInsert", "invalidateChainConfig",
    "slotUserCleared", "resetLfoTargetLabels", "chainComponentParamKey",
    "maybeConfirmForModule",
  ];
  for (const needle of mustContain) {
    if (!lifted.body.includes(needle)) {
      fail("applyChainComponentPick body is missing " + needle + " -- the sequence has been split up again");
    }
  }
}

// applyComponentSelectionConfirmed must be CALLED from exactly one function.
// Two textual matches are expected -- the definition, and the (single)
// function body that calls it twice on its two exit paths (gate-accepted
// callback, and the direct clear-with-no-gate path) -- so this counts CALL
// sites specifically, and checks both fall inside applyChainComponentPick.
const defMarker = "function applyComponentSelectionConfirmed(";
const defAt = src.indexOf(defMarker);
if (defAt < 0) fail("applyComponentSelectionConfirmed is gone");
const defCallTextAt = defAt + "function ".length; // where "applyComponentSelectionConfirmed(" itself starts

const callPattern = /applyComponentSelectionConfirmed\(/g;
let m; const callSites = [];
while ((m = callPattern.exec(src))) {
  if (m.index === defCallTextAt) continue; // the definition itself
  callSites.push(m.index);
}
if (callSites.length !== 2) {
  fail("expected exactly 2 calls to applyComponentSelectionConfirmed (both inside " +
       "applyChainComponentPick -- the gate-accepted callback and the direct " +
       "clear path), found " + callSites.length);
}
if (lifted) {
  for (const idx of callSites) {
    if (idx < lifted.start || idx > lifted.end) {
      fail("a call to applyComponentSelectionConfirmed sits outside applyChainComponentPick -- a second call site exists");
    }
  }
}

// applyComponentSelection (the picker resolution) must not call the confirm
// function directly any more -- only through applyChainComponentPick.
const selAt = src.indexOf("function applyComponentSelection(");
const selEnd = selAt >= 0 ? src.indexOf("\n}\n", selAt) : -1;
if (selAt < 0 || selEnd < 0) {
  fail("applyComponentSelection is gone");
} else {
  const selBody = src.slice(selAt, selEnd);
  if (selBody.includes("applyComponentSelectionConfirmed(")) {
    fail("applyComponentSelection still calls applyComponentSelectionConfirmed directly");
  }
  if (!selBody.includes("applyChainComponentPick(")) {
    fail("applyComponentSelection does not call applyChainComponentPick");
  }
}

// --------------------------------------------------------------------------
// 2. BEHAVIOURAL: run it. picked="" must reach the removal branch (no
//    feedback gate) and hand a "remove" shape to applyComponentSelectionConfirmed;
//    a non-empty pick with a real paramKey must reach the feedback gate.
// --------------------------------------------------------------------------

if (lifted) {
  function makeHarness() {
    const calls = {
      applyPickerChoiceToChain: [], invalidateChainConfig: [], resetLfoTargetLabels: [],
      maybeConfirmForModule: [], loadChainConfigFromSlot: [], setView: [],
      applyComponentSelectionConfirmed: [], host_get_module_metadata: [],
    };
    const chainConfigs = {};
    const slotUserCleared = {};
    const comps = { 0: [{ key: "fx1", label: "FX 1" }, { key: "fx2", label: "FX 2" }] };

    return { calls, chainConfigs, slotUserCleared, comps,
      apply: lifted.fn(
        chainConfigs,
        () => ({ synth: null, fx: [], midiFx: [] }),         // createEmptyChainConfig
        (choice, pending) => {                                 // withPendingChainInsert (real logic)
          if (!choice || !pending) return choice;
          if (pending.index >= pending.count) return choice;
          return { cfg: choice.cfg, replaced: null,
                    shape: { kind: "insert", section: pending.section, index: pending.index } };
        },
        (cfg, componentKey, moduleId) => {                     // applyPickerChoiceToChain (spy)
          calls.applyPickerChoiceToChain.push({ cfg, componentKey, moduleId });
          if (!moduleId) {
            return { cfg: { synth: cfg.synth, fx: [{ module: "reverb" }], midiFx: [] },
                      replaced: null, shape: { kind: "remove", section: "fx", index: 1 } };
          }
          return { cfg: { synth: cfg.synth, fx: cfg.fx, midiFx: cfg.midiFx },
                    replaced: "oldmod", shape: null };
        },
        (slotIndex) => { calls.invalidateChainConfig.push(slotIndex); },
        slotUserCleared,
        (cfg) => !!(cfg && (cfg.synth || (cfg.fx || []).some(Boolean) || (cfg.midiFx || []).some(Boolean))),
        () => { calls.resetLfoTargetLabels.push(true); },
        (componentKey, suffix) => componentKey + ":" + suffix,  // chainComponentParamKey
        (moduleId) => { calls.host_get_module_metadata.push(moduleId); return { id: moduleId }; },
        (msg) => {},                                             // host_log
        (meta, cb) => { calls.maybeConfirmForModule.push({ meta, cb }); },
        (slotIndex) => { calls.loadChainConfigFromSlot.push(slotIndex); },
        (view) => { calls.setView.push(view); },
        { CHAIN_EDIT: "chain_edit" },
        false,
        (slotIndex, paramKey, moduleId, comp, choice) => {
          calls.applyComponentSelectionConfirmed.push({ slotIndex, paramKey, moduleId, comp, choice });
        },
        (slotIndex, componentKey) => comps[slotIndex].findIndex((c) => c.key === componentKey),
        (slotIndex) => comps[slotIndex],
      ) };
  }

  // --- Case A: removal (None on a list position) --------------------------
  {
    const h = makeHarness();
    h.chainConfigs[0] = { synth: null, fx: [{ module: "reverb" }, { module: "delay" }], midiFx: [] };
    h.apply(0, "fx2", "", null);

    if (h.calls.applyPickerChoiceToChain.length !== 1)
      fail("removal: applyPickerChoiceToChain should be called exactly once");
    else if (h.calls.applyPickerChoiceToChain[0].moduleId !== "")
      fail("removal: applyPickerChoiceToChain was not called with picked=\"\"");

    if (h.calls.maybeConfirmForModule.length !== 0)
      fail("removal: the feedback gate must NOT run for a removal (moduleId is empty)");

    if (h.calls.applyComponentSelectionConfirmed.length !== 1) {
      fail("removal: applyComponentSelectionConfirmed should be called exactly once, directly");
    } else {
      const c = h.calls.applyComponentSelectionConfirmed[0];
      if (!c.choice || !c.choice.shape || c.choice.shape.kind !== "remove")
        fail("removal: the choice handed to applyComponentSelectionConfirmed does not carry a remove shape verb");
      if (c.moduleId !== "") fail("removal: moduleId reaching the confirm step should be empty");
      if (c.slotIndex !== 0) fail("removal: wrong slotIndex reached the confirm step");
      if (!c.comp || c.comp.key !== "fx2") fail("removal: wrong component reached the confirm step");
    }

    if (h.slotUserCleared[0] !== false)
      fail("removal: slotUserCleared should be false -- fx1 is still loaded after removing fx2");

    if (h.calls.invalidateChainConfig.length !== 1 || h.calls.invalidateChainConfig[0] !== 0)
      fail("removal: invalidateChainConfig(0) should run exactly once");

    if (h.calls.resetLfoTargetLabels.length !== 1)
      fail("removal: resetLfoTargetLabels should run exactly once");

    if (h.calls.loadChainConfigFromSlot.length !== 0 || h.calls.setView.length !== 0)
      fail("removal: the gate-decline path must not run for a removal");
  }

  // --- Case B: non-empty pick reaches the feedback gate --------------------
  {
    const h = makeHarness();
    h.chainConfigs[0] = { synth: null, fx: [null], midiFx: [] };
    h.apply(0, "fx1", "somemodule", null);

    if (h.calls.maybeConfirmForModule.length !== 1)
      fail("swap: a non-empty pick with a paramKey should reach the feedback gate exactly once");
    if (h.calls.applyComponentSelectionConfirmed.length !== 0)
      fail("swap: applyComponentSelectionConfirmed must not run before the gate callback fires");
    if (h.calls.host_get_module_metadata.length !== 1 || h.calls.host_get_module_metadata[0] !== "somemodule")
      fail("swap: module metadata should be looked up for the picked module");

    // Accept: the gate callback commits through applyComponentSelectionConfirmed.
    h.calls.maybeConfirmForModule[0].cb(true);
    if (h.calls.applyComponentSelectionConfirmed.length !== 1)
      fail("swap accepted: applyComponentSelectionConfirmed should run exactly once after accept");
    else if (h.calls.applyComponentSelectionConfirmed[0].moduleId !== "somemodule")
      fail("swap accepted: wrong moduleId reached the confirm step");
  }

  // --- Case C: decline reloads the slot and reverts slotUserCleared --------
  {
    const h = makeHarness();
    h.chainConfigs[0] = { synth: null, fx: [null], midiFx: [] };
    h.apply(0, "fx1", "somemodule", null);
    h.calls.maybeConfirmForModule[0].cb(false);

    if (h.calls.applyComponentSelectionConfirmed.length !== 0)
      fail("swap declined: applyComponentSelectionConfirmed must not run");
    if (h.calls.loadChainConfigFromSlot.length !== 1 || h.calls.loadChainConfigFromSlot[0] !== 0)
      fail("swap declined: the slot should be reloaded from the DSP");
    if (h.slotUserCleared[0] !== false)
      fail("swap declined: slotUserCleared should be reset to false");
    if (h.calls.setView.length !== 1 || h.calls.setView[0] !== "chain_edit")
      fail("swap declined: should return to the chain editor view");
  }
}

if (failures > 0) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS");
'
