#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE KNOB GRID LOADS A MODULE'S WIDGETS ON EVERY CHAIN, NOT ONLY A SLOT'S.
#
# tickComponentWidgets read the module's chain_params with
# getComponentChainParams(slot, comp) -- the slot chain's accessor -- while the
# grid's `comp` is the EDITOR key: "fx2" in a slot, "master_fx:fx2" in Master FX
# or a send, "bus1:fx2" on a bus. slotChainTarget's key rule answers null for
# both prefixed forms, so the read came back [] and ensureComponentWidgets,
# which rightly treats an empty chain_params as "not answered yet", retried
# forever without loading canvas.js. A module with a custom: widget drew plain
# dials in Master FX and its own picture in a slot. The device log said
#
#     widgets: id="pushnpull" params=0 vizKinds=[]
#
# four times a second for as long as the grid was open -- the id resolved (that
# read goes through componentModuleIdKey, which knows all three chains), the
# params never.
#
# Two things are pinned: the resolver answers each key form from the right
# chain (run, not read), and the widget tick asks the resolver.

file="src/shadow/shadow_ui.js"
command -v node >/dev/null 2>&1 || { echo "FAIL: node is required" >&2; exit 1; }

grid=$(awk '/^function gridComponentChainParams\(/,/^}/' "$file")
tick=$(awk '/^function tickComponentWidgets\(/,/^}/' "$file")
[ -n "$grid" ] || { echo "FAIL: gridComponentChainParams not found in $file" >&2; exit 1; }
[ -n "$tick" ] || { echo "FAIL: tickComponentWidgets not found in $file" >&2; exit 1; }

if ! printf '%s\n' "$tick" | grep -q 'ensureComponentWidgets(id, gridComponentChainParams(slot, comp))'; then
  echo "FAIL: tickComponentWidgets does not read chain_params through gridComponentChainParams;" >&2
  echo "      a Master FX, send or bus position then reads [] and never loads its widgets" >&2
  exit 1
fi

printf '%s\n' "$grid" | node -e '
const fs = require("fs");
const src = fs.readFileSync(0, "utf8");
let failures = 0;
const check = (cond, m) => { if (cond) console.log("  ok  " + m); else { console.log("FAIL: " + m); failures++; } };

const calls = [];
const make = new Function(
  "BusModel", "busChainTarget", "chainTargetChainParams",
  "masterFxIndexFromComponentKey", "getMasterFxChainParams", "getComponentChainParams",
  src + "; return gridComponentChainParams;");

const grid = make(
  { parseBusComponentKey: (k) => {
      const m = /^bus(\d+):fx(\d+)$/.exec(k || "");
      return m ? { bus: Number(m[1]) - 1, fx: Number(m[2]) - 1 } : null;
    } },
  (bus, slot) => ({ kind: "bus", bus, slot }),
  (target, position) => { calls.push(["bus", target.bus, target.slot, position]); return ["bus"]; },
  (k) => { const m = /^(?:master_fx|send\d+):fx(\d+)$/.exec(k || ""); return m ? Number(m[1]) - 1 : -1; },
  (i) => { calls.push(["master", i]); return ["master"]; },
  (slot, k) => { calls.push(["slot", slot, k]); return ["slot"]; });

const last = () => JSON.stringify(calls[calls.length - 1]);

check(grid(2, "fx1")[0] === "slot" && last() === JSON.stringify(["slot", 2, "fx1"]),
      "a slot position is read from the slot chain, key unchanged");
check(grid(0, "master_fx:fx2")[0] === "master" && last() === JSON.stringify(["master", 1]),
      "a Master FX position is read from the master chain, by position");
check(grid(0, "send1:fx1")[0] === "master" && last() === JSON.stringify(["master", 0]),
      "a send position is read from the same FX-bus chain");
check(grid(3, "bus1:fx2")[0] === "bus" && last() === JSON.stringify(["bus", 0, 3, "fx2"]),
      "a bus insert is read from its bus, at the real slot, by bare position");
check(grid(1, "synth")[0] === "slot", "a synth position still falls to the slot chain");

process.exit(failures ? 1 : 0);
'
echo "PASS: the knob grid reads chain_params from the chain it is on"
