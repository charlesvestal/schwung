#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A module's in-grid widgets must register again after ANOTHER module has been
# visited in between.
#
# tickComponentWidgets began `if (widgetModuleLoaded) return;`. The latch holds
# a module ID, so any truthy value read as "resolved, nothing to ask" -- sound
# only if the latch can only describe the component on screen, which it cannot.
# ensureComponentWidgets calls clearWidgets() and sets the latch for whichever
# module reaches it, from either call site.
#
# So: open a module that declares no custom kind, and the registry is emptied
# and the latch set to its id. From then on this function returned on its first
# line for every component visited afterwards, and nothing registered again.
# The registry is process-global, so a module that HAS a widget then drew the
# detector's dials instead -- silently, with no log line, until reboot.
# Observed on device 2026-09-07: hinge registered at 12:25, dr32 cleared it at
# 12:31, and hinge drew dials for every entry after that.
#
# Two things are asserted, because the obvious fix breaks the other one:
# deleting the guard makes re-entry work and costs a ~2.8ms `_module` IPC read
# on EVERY frame, against a 1.68ms whole-page render. The guard has to stay and
# ask the narrower question.

file="src/shadow/shadow_ui.js"

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

fn=$(awk '/^function tickComponentWidgets\(/,/^}/' "$file")
if [ -z "$fn" ]; then
  echo "FAIL: tickComponentWidgets not found in $file" >&2
  exit 1
fi

# The retry throttle is read from the source rather than restated, so raising it
# cannot silently invalidate the read-cost assertion below.
retry_ticks=$(sed -n 's/^const WIDGET_RETRY_TICKS = \([0-9]*\);.*/\1/p' "$file")
if [ -z "$retry_ticks" ]; then
  echo "FAIL: WIDGET_RETRY_TICKS not found in $file" >&2
  exit 1
fi

printf '%s' "$fn" | node -e '
const fs = require("fs");
const src = fs.readFileSync(0, "utf8");
const RETRY = Number(process.argv[1]);
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

/* The scenario is a sequence of components, so the harness has to hold the
 * latch state the function mutates. Declaring it here rather than lifting it
 * keeps the scaffolding explicit -- and a rename in the source fails loudly
 * instead of quietly creating a fresh global. */
const preamble = `
  let widgetModuleLoaded = "";
  let widgetAttemptedSig = "";
  let widgetResolvedSig = "";
  let widgetRetryTick = 0;
`;

/* Stands in for the real ensureComponentWidgets, reproducing the only two
 * behaviours this test depends on: it wipes the process-global registry, and
 * it latches the id it was given. Defined inside the same source string so it
 * shares the latch binding, exactly as the real pair do. */
const stub = `
  function ensureComponentWidgets(id, chainParams) {
    if (!id) return;                                   /* tri-state: unresolved */
    if (id === widgetModuleLoaded) return;
    if (!Array.isArray(chainParams) || chainParams.length === 0) return;
    registry.length = 0;                               /* clearWidgets() */
    widgetModuleLoaded = id;
    if (chainParams.some((p) => p && p.viz && String(p.viz.kind).startsWith("custom:")))
      registry.push(id);
  }
`;

let slot = 0, comp = "", moduleReads = 0;
const registry = [];
const MODULES = {
  "0:synth": { id: "hinge", params: [{ viz: { kind: "custom:hinge_wave" } }] },
  "1:synth": { id: "dr32",  params: [{ viz: {} }] },
};
const here = () => MODULES[`${slot}:${comp}`] || { id: "", params: [] };

const make = new Function(
  "paramPagesSlot", "paramPagesComponent", "getSlotParam",
  /* componentModuleIdKey, not getComponentParamPrefix: the key that NAMES the
     module differs per chain -- a slot chain says "fx1_module", an FX bus says
     "master_fx:fx1:name" -- and the grid may be on any of them, so the spelling
     is resolved rather than composed at the call site. */
  "componentModuleIdKey", "getComponentChainParams",
  "WIDGET_RETRY_TICKS", "registry", "debugLog",
  preamble + stub + src +
  "; return { tickComponentWidgets, ensureComponentWidgets, latch: () => widgetModuleLoaded };");

const api = make(
  () => slot,
  () => comp,
  () => { moduleReads++; return here().id; },
  () => "synth_module",
  () => here().params,
  RETRY, registry, () => {});

const visit = (s, c, frames) => {
  slot = s; comp = c;
  for (let i = 0; i < frames; i++) api.tickComponentWidgets();
};

/* 1. hinge, which declares a custom kind. */
visit(0, "synth", 4);
if (!registry.includes("hinge"))
  fail("hinge did not register its widget on first entry");

/* 2. dr32, which declares none. Emptying the registry is correct and
 *    deliberate -- a widget left behind would outlive its module. */
visit(1, "synth", 4);
if (registry.includes("hinge"))
  fail("visiting dr32 left hinge widget registered; a stale widget outlives its module");

/* 3. back to hinge. THE REGRESSION. */
visit(0, "synth", 4);
if (!registry.includes("hinge"))
  fail("hinge did not re-register after visiting a module with no custom widget -- " +
       "its cells fall through to the detector and draw dials, silently, until reboot");

/* 3b. THE SEQUENCE OBSERVED ON DEVICE, which reaches the same guard by the
 *     other door: the list editor (loadHierarchyLevel) registers a component
 *     directly, without this tick ever running. That is how dr32 came to own
 *     the latch at 12:31 while the knob grid was the only thing that could
 *     have put hinge back. */
api.ensureComponentWidgets("dr32", [{ viz: {} }]);
if (registry.includes("hinge"))
  fail("harness: the list-editor path did not clear the registry");
/* ONE frame: the recovery must be immediate, not throttled. The signature
 * changed, so this is a new situation, and ~250ms of dials from the detector
 * before the module supplies its own art is the "an unresolved answer must not
 * become a picture" rule broken with extra steps. */
visit(0, "synth", 1);
if (!registry.includes("hinge"))
  fail("hinge did not re-register on the FIRST frame after the LIST EDITOR loaded " +
       "another module; the knob grid either never re-asks, or waits out the throttle");

/* 4. and the guard still has to close, or every frame pays an IPC read. */
moduleReads = 0;
visit(0, "synth", 120);
if (moduleReads !== 0)
  fail("staying on a resolved component cost " + moduleReads + " `_module` IPC reads " +
       "across 120 frames, expected 0 (~2.8ms each, vs a 1.68ms whole-page render)");

/* 5. an unresolved id must not close the guard -- the tri-state rule, one
 *    level down. Recording the key regardless of whether the latch actually
 *    took would re-create the bug in a new place. */
MODULES["2:synth"] = { id: "", params: [] };
visit(2, "synth", 1);
moduleReads = 0;
visit(2, "synth", RETRY * 2 + 2);
if (moduleReads === 0)
  fail("an unresolved module id stopped the retry; a read that did not answer " +
       "must never become a cached verdict");

console.log("PASS: widget latch is per-component, and still costs nothing once resolved");
' "$retry_ticks"
