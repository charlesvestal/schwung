#!/bin/bash
#
# A component that ships a web_ui.html must render its panel, and must render
# it where signal flow puts it.
#
# THE PANEL WAS NEVER MISSING -- it was FOLDED.  The manager sent custom_ui for
# midi_fx1 with the right URL, the client stored it, the section rendered, and
# renderComponentSection returned at `if (isCollapsed)` one branch ABOVE the
# custom-UI branch.  Slot state hard-coded `collapsed: { synth: false,
# fx1: true, fx2: true, midi_fx1: true }`, a default written for generated rows
# where folding saves space, so the whole Stacks UI sat behind a one-line
# disclosure triangle roughly 3000px below the synth panel, with nothing on
# screen to say a panel existed at all.  Every layer reported success.
#
# Two invariants, because fixing only the first still leaves it far off-screen:
#   1. fold state is DERIVED for an untouched section -- a component with a
#      panel opens -- and records only what the user actually chose.
#   2. ONE draw order, signal flow, shared by both render paths.  They each
#      carried their own list, and the custom-synth path's put midi_fx1 last.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JS="$ROOT/schwung-manager/static/remote-ui.js"
[ -f "$JS" ] || { echo "FAIL: $JS not found"; exit 1; }

# --- 1. Behaviour: run the real sectionCollapsed, not a grep of it. ---------
node - "$JS" <<'NODE'
const fs = require("fs");
const src = fs.readFileSync(process.argv[2], "utf8");

const m = src.match(/function sectionCollapsed\(state, compKey, leadKey\)\s*\{[\s\S]*?\n    \}/);
if (!m) { console.error("FAIL: sectionCollapsed(state, compKey, leadKey) not found"); process.exit(1); }
const sectionCollapsed = new Function("return (" + m[0] + ")")();

function check(cond, msg) { if (!cond) { console.error("FAIL: " + msg); process.exit(1); } }

// The bug, exactly: a MIDI FX with a panel, nothing touched by the user.
check(sectionCollapsed({ collapsed: {}, customUI: { midi_fx1: "/x.html" } }, "midi_fx1", "synth") === false,
      "a component that ships a panel must NOT default to collapsed");

// A component with no panel still folds -- that default was never the problem.
check(sectionCollapsed({ collapsed: {}, customUI: {} }, "fx1", "synth") === true,
      "a generated-rows FX section should still default to folded");

// The lead position opens, so a slot is not a stack of closed triangles.
check(sectionCollapsed({ collapsed: {}, customUI: {} }, "synth", "synth") === false,
      "the lead component must default to open");

// An explicit user choice WINS in both directions -- including folding a panel
// away, which is the whole reason the state is stored at all.
check(sectionCollapsed({ collapsed: { midi_fx1: true }, customUI: { midi_fx1: "/x.html" } }, "midi_fx1", "synth") === true,
      "a user who folds a panel must keep it folded");
check(sectionCollapsed({ collapsed: { fx1: false }, customUI: {} }, "fx1", "synth") === false,
      "a user who opens a generated section must keep it open");

// Master FX reuses the same helper with its own lead key.
check(sectionCollapsed({ collapsed: {}, customUI: { "master_fx:fx3": "/x.html" } },
                       "master_fx:fx3", "master_fx:fx1") === false,
      "a Master FX position with a panel must default to open");

// customUI may be absent entirely on a freshly-built state.
check(sectionCollapsed({ collapsed: {} }, "fx2", "synth") === true,
      "a state with no customUI map must not throw");

console.log("  sectionCollapsed: 7 cases OK");
NODE

# --- 2. Structure: one draw order, shared, signal flow. ---------------------
order="$(grep -n 'var COMPONENT_ORDER *=' "$JS" || true)"
[ -n "$order" ] || { echo "FAIL: COMPONENT_ORDER not declared"; exit 1; }
echo "$order" | grep -q '"midi_fx1", *"synth"' || {
    echo "FAIL: COMPONENT_ORDER must place midi_fx1 before synth (signal flow)"; exit 1; }

uses="$(grep -c 'COMPONENT_ORDER\[k\]' "$JS" || true)"
[ "$uses" -ge 2 ] || {
    echo "FAIL: both render paths must iterate COMPONENT_ORDER (found $uses)"; exit 1; }

# A second hard-coded order is how the two paths drifted apart the first time.
if grep -q 'compOrder *= *\[' "$JS"; then
    echo "FAIL: a second hard-coded component order reappeared — use COMPONENT_ORDER"; exit 1
fi

# The fold default must not be restated as a literal in slot state.
if grep -qE 'collapsed: *\{ *synth:|collapsed: *\{ *"master_fx' "$JS"; then
    echo "FAIL: fold state is hard-coded again — it must start empty and be derived"; exit 1
fi

echo "PASS: a component that ships a panel renders it, open, in signal-flow order"
