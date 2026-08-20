#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# An LFO routing does not outlive the module it was aimed at.
#
# A chain position keeps its NAME across a module change -- "fx1" is "fx1"
# whatever is loaded there -- so nothing about replacing the module invalidates
# a routing that names it. The DSP drops the position modulation ENTRIES when
# it unloads, but the LFO still holds target:"fx1" and target_param:"room_size"
# and keeps emitting. Reported from hardware: replace a modulated FX and the
# LFO editor shows it targeting a parameter of a module that is no longer in
# the chain.
#
# The dangerous half is the inverse. If the module that lands in that position
# declares a param of the same name -- mix, gain, feedback are not rare -- the
# LFO silently starts modulating the NEW module. Audible, and attributable to
# nothing the user did.
#
# The whole-section rewrite (writeChainOrder) already clears a routing whose
# module left the chain; that is pinned in test_chain_order_persist.sh. This
# file pins the OTHER way a module leaves a position: the picker swap, which
# writes one <id>:module and never reaches that code at all.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { parseId as parseChainId, removeAt as chainRemoveAt } from "./src/shared/chain_model.mjs";
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

function lift(name, deps) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
}

const mk = (name, deps, args) => { const f = lift(name, deps); return f ? f(...args) : null; };

const chainComponentId = mk("chainComponentId", [], []);
const getChainComponentModule = mk("getChainComponentModule",
  ["parseChainId", "chainComponentId"], [parseChainId, chainComponentId]);
const setChainComponentModule = mk("setChainComponentModule",
  ["parseChainId", "chainComponentId"], [parseChainId, chainComponentId]);
if (!chainComponentId || !getChainComponentModule || !setChainComponentModule) process.exit(1);

const makeClear = lift("clearLfoRoutingForComponent", ["getSlotParam", "setSlotParam"]);
const makeChoice = lift("applyPickerChoiceToChain",
  ["chainComponentId", "parseChainId", "getChainComponentModule", "chainRemoveAt",
   "setChainComponentModule"]);
if (!makeClear || !makeChoice) process.exit(1);
const applyPickerChoiceToChain = makeChoice(chainComponentId, parseChainId,
  getChainComponentModule, chainRemoveAt, setChainComponentModule);

function device(state) {
  const writes = [];
  const reads = [];
  const get = (slot, key) => { reads.push(key); return state[key] !== undefined ? state[key] : ""; };
  const set = (slot, key, val) => { writes.push(key + "=" + val); state[key] = String(val); return true; };
  return { get, set, writes, reads, state };
}
const clearFor = (state, slot, id) => {
  const d = device(state);
  makeClear(d.get, d.set)(slot, id);
  return d;
};
const chain = (fx, synth) => ({
  midiFx: [], synth: synth ? { module: synth, params: {} } : null,
  fx: fx.map((m) => (m ? { module: m, params: {} } : null)),
});

/* ---- the helper itself -------------------------------------------------- */

/* 1. A routing naming the replaced position is cleared, BOTH halves. Leaving
      target_param behind leaves the dead module param name stored, ready for
      the next write of target alone to revive it. */
{
  const d = clearFor({ "lfo1:target": "fx1", "lfo1:target_param": "room_size" }, 0, "fx1");
  if (d.state["lfo1:target"] !== "")
    fail("the routing survived the replacement: " + d.state["lfo1:target"]);
  if (d.state["lfo1:target_param"] !== "")
    fail("target_param still names " + d.state["lfo1:target_param"] +
         ", a param of the module that just left");
}

/* 2. A routing aimed somewhere ELSE is untouched -- the helper clears one
      position, not every routing it can see. */
{
  const d = clearFor({ "lfo1:target": "fx2", "lfo1:target_param": "mix",
                       "lfo2:target": "synth", "lfo2:target_param": "cutoff" }, 0, "fx1");
  if (d.writes.length !== 0)
    fail("routings aimed elsewhere were cleared: " + d.writes.join(" "));
}

/* 3. BOTH LFOs are covered. Two is the whole table (LFO_COUNT), and a loop
      that stops at the first is the shape this codebase keeps growing. */
{
  const d = clearFor({ "lfo1:target": "fx1", "lfo1:target_param": "a",
                       "lfo2:target": "fx1", "lfo2:target_param": "b" }, 0, "fx1");
  if (d.state["lfo2:target"] !== "" || d.state["lfo2:target_param"] !== "")
    fail("only the first LFO was cleared: " + d.writes.join(" "));
}

/* 4. The synth is a component like any other here: swapping it out has to take
      routings aimed at it too. */
{
  const d = clearFor({ "lfo1:target": "synth", "lfo1:target_param": "cutoff" }, 0, "synth");
  if (d.state["lfo1:target"] !== "") fail("a synth routing survived a synth swap");
}

/* 5. midi_fx positions are named the same way and are reached by the same
      path -- the id is a string, so nothing about it is FX-only. */
{
  const d = clearFor({ "lfo1:target": "midi_fx3", "lfo1:target_param": "rate" }, 0, "midi_fx3");
  if (d.state["lfo1:target"] !== "") fail("a MIDI FX routing survived a swap");
}

/* 6. Nothing to clear costs two reads and no writes. This runs on a user
      gesture, but a helper that writes unconditionally would reload routings
      that were never stale. */
{
  const d = clearFor({}, 0, "fx1");
  if (d.writes.length !== 0) fail("wrote with nothing routed: " + d.writes.join(" "));
  if (d.reads.length > 2) fail("read more than the two LFOs: " + d.reads.join(" "));
}

/* ---- what the picker hands the caller ----------------------------------- */

/* 7. A swap reports the module it REPLACED, which is how the caller knows
      whether any routing aimed at that position is still meaningful. */
{
  const c = applyPickerChoiceToChain(chain(["psxverb"]), "fx1", "spacedelay");
  if (c.replaced !== "psxverb") fail("a swap did not report what it replaced: " + c.replaced);
  if (c.shape) fail("a swap asked for a shape change");
}

/* 8. A REMOVAL reports null rather than a module id: it asks the DSP for a
      `remove`, and the DSP decides the routing there -- it re-aims the string
      keys across the permutation and can tell a module that LEFT from one that
      only moved down, which one position in isolation cannot. Reporting a
      module id here would clear the routing of a module that MOVED. */
{
  const c = applyPickerChoiceToChain(chain(["psxverb", "delay"]), "fx1", "");
  if (c.replaced !== null)
    fail("a removal reported a replaced module (" + c.replaced +
         "), which would clear the routing of a module that only moved");
  if (!c.shape || c.shape.kind !== "remove" || c.shape.section !== "fx")
    fail("a removal did not ask the DSP to remove the position");
}

/* 9. Clearing the SYNTH is a replacement, not a removal -- there is nothing to
      renumber, so nobody else will clear the routing. */
{
  const c = applyPickerChoiceToChain(chain([], "sf2"), "synth", "");
  if (c.replaced !== "sf2") fail("clearing the synth did not report it: " + c.replaced);
  if (c.shape) fail("clearing the synth asked for a shape change");
}

/* 10. An EMPTY position filled for the first time reports "" -- distinct from
       the removal null, and still a change, so a routing left pointing at that
       empty position from an earlier life does not survive into the new
       module. */
{
  const c = applyPickerChoiceToChain(chain([]), "fx1", "freeverb");
  if (c.replaced !== "") fail("filling an empty position reported " + c.replaced);
}

/* ---- the decision the caller makes with it ------------------------------ */

const pickerReplacedModule = mk("pickerReplacedModule", [], []);
if (!pickerReplacedModule) process.exit(1);

/* 11. Each of the four answers, because each is a different decision. */
{
  const cases = [
    [null, "delay", false, "a removal must defer to the DSP, which alone can tell " +
                           "a module that LEFT from one that only moved down"],
    [undefined, "delay", false, "an unreported replacement must not clear a routing"],
    ["psxverb", "spacedelay", true, "a real swap must clear the routing"],
    ["psxverb", "psxverb", false, "re-picking the SAME module is a reload, and the routing " +
                                  "still names the module the user routed"],
    ["Freeverb", "freeverb", false, "module ids arrive from the picker and the DSP in " +
                                    "different cases and must not read as a swap"],
    ["", "freeverb", true, "filling a position that was empty is still a change: a routing " +
                           "left over from its previous occupant would land on this module"],
    ["psxverb", "", true, "clearing the synth is a replacement -- nothing renumbers, so " +
                          "nobody else will clear the routing"],
  ];
  for (const [was, now, want, why] of cases) {
    if (pickerReplacedModule(was, now) !== want)
      fail("pickerReplacedModule(" + JSON.stringify(was) + ", " + JSON.stringify(now) +
           ") should be " + want + ": " + why);
  }
}

/* ---- and that the caller actually calls it ------------------------------ */

/* 12. BEFORE the module write. After it the position has been reloaded and its
       modulation entries are gone, so nothing is left to say the routing was
       ever valid -- and the LFO keeps its aim regardless. */
{
  const at = src.indexOf("function applyComponentSelectionConfirmed(");
  const end = src.indexOf("\n}\n", at);
  const body = at >= 0 ? src.slice(at, end) : "";
  const clear = body.indexOf("clearLfoRoutingForComponent(");
  const write = body.indexOf("setSlotParam(slotIndex, paramKey");
  if (clear < 0)
    fail("applyComponentSelectionConfirmed never clears the routing of a swapped module");
  else if (write < 0)
    fail("could not find the module write in applyComponentSelectionConfirmed");
  else if (clear > write)
    fail("the routing is cleared AFTER the module write, which reads and clears nothing");
}

if (failures) process.exit(1);
console.log("PASS: an LFO routing does not outlive the module it named — a picker swap " +
            "clears target AND target_param for that position, before the module write, " +
            "for both LFOs and for synth/fx/midi_fx alike; a removal defers to " +
            "the DSP permutation so a module that only MOVED keeps its routing");
'
