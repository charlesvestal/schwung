#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# How a chain edit reaches the DSP -- and the promise that nothing is RELOADED
# to make it happen.
#
# This file used to test writeChainOrder, which pushed a whole section as a run
# of `<id>:module` writes and then carried each surviving module its opaque
# `:state`, its modulation base and its LFO routing across the renumber. Every
# one of those module writes unloads the position and dlopen()s a fresh
# instance, so a reorder rebuilt the modules it moved and a removal rebuilt
# everything downstream. The carry made that survivable for anything a state
# blob can express -- and a state blob cannot express a running arpeggiator
# phase or the tail ringing in a delay. Reported on hardware as "we can't change
# phase of a running module by just adding a new one".
#
# The DSP permutes its per-position arrays instead (tests/host/test_chain_permute
# covers that half), so what this file pins is the JS side of the contract:
#
#   - each gesture is ONE verb naming positions, not a run of module writes;
#   - and specifically, NO `:module` write goes out for a module that only
#     moved. That is the regression that would silently bring the reloads back,
#     and it would be inaudible in any test that only checked the final order.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { parseId as parseChainId, moveBy as chainMoveBy, removeAt as chainRemoveAt,
         insertAt as chainInsertAt } from "./src/shared/chain_model.mjs";
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* Lift a top-level function out of shadow_ui.js and hand it its dependencies
   as parameters. The file cannot be imported -- it is a device UI module full
   of host globals -- but a function that closes over a few named things can be
   RUN, which is the difference between pinning the source text and pinning the
   behaviour. */
function lift(name, deps) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
}

const chainComponentId = lift("chainComponentId", [])();
const chainSectionPrefix = lift("chainSectionPrefix", [])();

/* A recording slot. Every write is kept, so a test can assert on what did NOT
   go out as easily as on what did. */
function device() {
  const writes = [];
  return { writes, set: (slot, key, val) => { writes.push(key + "=" + val); return true; } };
}
const makeShape = lift("writeChainShape",
  ["chainSectionPrefix", "setSlotParam", "invalidateChainConfig"]);
if (!makeShape) process.exit(1);
const run = (op) => {
  const d = device();
  makeShape(chainSectionPrefix, d.set, () => {})(0, op);
  return d.writes;
};

/* ---- the wire ---------------------------------------------------------- */

/* 1. One verb per gesture, and the positions are ONE-BASED -- the ids
      everything else speaks are ("fx2"), so a caller never converts and an
      off-by-one here would edit the neighbour of what the user selected. */
{
  const cases = [
    [{ kind: "insert", section: "fx", index: 0 }, "fx:insert=1"],
    [{ kind: "insert", section: "midiFx", index: 0 }, "midi_fx:insert=1"],
    [{ kind: "remove", section: "fx", index: 2 }, "fx:remove=3"],
    [{ kind: "remove", section: "midiFx", index: 1 }, "midi_fx:remove=2"],
    [{ kind: "move", section: "fx", index: 0, to: 2 }, "fx:move=1>3"],
    [{ kind: "move", section: "midiFx", index: 2, to: 0 }, "midi_fx:move=3>1"],
  ];
  for (const [op, want] of cases) {
    const w = run(op);
    if (w.join(" ") !== want)
      fail("expected [" + want + "], got [" + w.join(" ") + "]");
  }
}

/* 2. NOT A MODULE WRITE. This is the whole point: `<id>:module` unloads the
      position and dlopens a fresh instance, so a shape change expressed that
      way rebuilds every module it touches. */
{
  for (const op of [{ kind: "insert", section: "fx", index: 0 },
                    { kind: "remove", section: "fx", index: 0 },
                    { kind: "move", section: "fx", index: 0, to: 1 }]) {
    const w = run(op);
    if (w.some((x) => x.includes(":module=")))
      fail("a " + op.kind + " wrote a module id, which RELOADS the position: " + w.join(" "));
  }
}

/* 3. Nothing is carried, because nothing is destroyed. The instance keeps
      running, so its state blob, its modulation entries -- and the base values
      inside them -- and the routings that name it are all still live. A carry
      here would now be writing over the real thing with a snapshot of it. */
{
  for (const op of [{ kind: "insert", section: "midiFx", index: 0 },
                    { kind: "remove", section: "midiFx", index: 0 },
                    { kind: "move", section: "midiFx", index: 1, to: 0 }]) {
    const w = run(op);
    if (w.some((x) => /:state=|:base=|^lfo\d:target/.test(x)))
      fail("a " + op.kind + " carried state or routing that never needed carrying: " +
           w.join(" "));
  }
}

/* 4. An unknown or absent op writes nothing rather than something arbitrary. */
{
  for (const op of [null, undefined, {}, { kind: "insert" }, { kind: "wat", section: "fx", index: 0 }]) {
    const w = run(op);
    if (w.length) fail("a malformed shape op wrote " + w.join(" "));
  }
}

/* 5. The editor cache is dropped. A pure reorder writes no module ids at all,
      so the slot module signature is byte-identical across it and nothing else
      would notice -- the editor would keep drawing the old order. */
{
  let dropped = 0;
  const d = device();
  makeShape(chainSectionPrefix, d.set, () => { dropped++; })(
    0, { kind: "move", section: "fx", index: 0, to: 1 });
  if (dropped !== 1)
    fail("a reorder left the editor cached view of the slot marked fresh");
}

/* ---- the gestures that produce them ------------------------------------ */

const mods = (names) => names.map((n) => (n ? { module: n, params: {} } : null));
const order = (l) => l.map((m) => (m ? m.module : "-")).join(",");

/* 6. Shift+jog and the picker Move rows both go through moveChainComponent,
      which must send the move the MODEL agreed to -- not the one it was asked
      for. A refused move (at the end of a section) must send nothing at all. */
{
  const makeMove = lift("moveChainComponent",
    ["chainConfigs", "chainComponentId", "parseChainId", "chainMoveBy", "writeChainShape",
     "resetLfoTargetLabels", "invalidateKnobContextCache"]);
  const move = (cfg, key, delta) => {
    const ops = [];
    const cfgs = [cfg];
    const moved = makeMove(cfgs, chainComponentId, parseChainId, chainMoveBy,
                           (s, op) => ops.push(op), () => {}, () => {})(0, key, delta);
    return { moved, ops, cfg: cfgs[0] };
  };
  const r = move({ midiFx: [], synth: null, fx: mods(["a", "b", "c"]) }, "fx2", 1);
  if (!r.moved || order(r.cfg.fx) !== "a,c,b") fail("6: the model did not move fx2");
  if (r.ops.length !== 1 || r.ops[0].kind !== "move" ||
      r.ops[0].index !== 1 || r.ops[0].to !== 2)
    fail("6: moving fx2 right sent " + JSON.stringify(r.ops));

  const stuck = move({ midiFx: [], synth: null, fx: mods(["a", "b"]) }, "fx2", 1);
  if (stuck.moved) fail("6: moving the last FX right reported a move");
  if (stuck.ops.length) fail("6: a refused move still wrote to the DSP");

  /* The one the DSP cannot see for itself: an EMPTY position -- the hole a `+`
     box materialises -- is not a module and does not move. */
  const hole = move({ midiFx: [], synth: null, fx: [null, mods(["b"])[0]] }, "fx1", 1);
  if (hole.moved || hole.ops.length) fail("6: a hole was dragged around");
}

/* 7. The picker: None on a list position asks for a remove at that position,
      and a swap asks for no shape change at all -- resequencing a patch the
      user only meant to retouch would change the signal path behind their
      back. */
{
  const setChainComponentModule = lift("setChainComponentModule",
    ["chainComponentId", "parseChainId"])(chainComponentId, parseChainId);
  const getChainComponentModule = lift("getChainComponentModule",
    ["chainComponentId", "parseChainId"])(chainComponentId, parseChainId);
  const choose = lift("applyPickerChoiceToChain",
    ["chainComponentId", "parseChainId", "chainRemoveAt", "setChainComponentModule",
     "getChainComponentModule"])(chainComponentId, parseChainId, chainRemoveAt,
                                 setChainComponentModule, getChainComponentModule);
  const rm = choose({ midiFx: mods(["arp", "chord"]), synth: null, fx: [] }, "midi_fx2", "");
  if (!rm.shape || rm.shape.kind !== "remove" || rm.shape.section !== "midiFx" ||
      rm.shape.index !== 1)
    fail("7: None on midi_fx2 sent " + JSON.stringify(rm.shape));
  const sw = choose({ midiFx: [], synth: null, fx: mods(["a", "b"]) }, "fx1", "delay");
  if (sw.shape) fail("7: a swap asked for a shape change: " + JSON.stringify(sw.shape));
}

/* 8. And the `+` box: only an insert that has something to its RIGHT needs the
      DSP to open a hole. An append does not -- nothing shifts, and the module
      write already grows the section. */
{
  const withPending = lift("withPendingChainInsert", [])();
  const choice = { cfg: {}, shape: null, replaced: "" };
  const head = withPending(choice, { section: "midiFx", index: 0, count: 2 });
  if (!head.shape || head.shape.kind !== "insert" || head.shape.index !== 0)
    fail("8: a head insert did not ask the DSP to open a hole");
  if (head.replaced !== null)
    fail("8: an insert reported a replaced module, which would clear a routing " +
         "belonging to the module it pushed along");
  const append = withPending(choice, { section: "fx", index: 3, count: 3 });
  if (append.shape) fail("8: an append asked for a hole it does not need");
  if (withPending(choice, null) !== choice)
    fail("8: an ordinary pick was rewritten as an insert");
}

/* 9. THE REGRESSION. An existing saved slot with exactly fx1 and fx2 still
      loads with both, in order, with the same modules. Nothing about the
      variable-length chain or the permutation may migrate a patch someone
      already has. */
{
  const makeLoad = lift("loadChainConfigFromSlot",
    ["chainConfigs", "createEmptyChainConfig", "getSlotParam", "CHAIN_CAP",
     "fxDisplayNameCache", "fxDisplayNameSkip", "fxDisplayNameBackoff", "chainConfigFresh"]);
  if (makeLoad) {
    const state = { synth_module: "sf2", fx_count: "2",
                    fx1_module: "freeverb", fx2_module: "cloudseed" };
    const cfgs = [{ midiFx: [], synth: null, fx: [] }];
    const cfg = makeLoad(cfgs, () => ({ midiFx: [], synth: null, fx: [] }),
                         (s, k) => (state[k] !== undefined ? state[k] : ""),
                         { midiFx: 8, fx: 8 }, {}, {}, {}, [])(0);
    const got = cfg.fx.map((f) => (f ? f.module : "-")).join(",");
    if (got !== "freeverb,cloudseed")
      fail("9: a saved two-FX slot no longer loads as it was: " + got);
    if (!cfg.synth || cfg.synth.module !== "sf2") fail("9: the synth did not survive the load");
  }
}

/* 10. And writeChainOrder really is gone, along with the carries that only
       existed because a renumber used to destroy things. Leaving a second
       mechanism that also claims to move a module is worse than either alone:
       the two would disagree about which one renumbered the DSP. */
{
  if (/function writeChainOrder\(/.test(src))
    fail("10: writeChainOrder is back alongside the permutation verbs");
  const confirm = src.slice(src.indexOf("function applyComponentSelectionConfirmed("));
  const body = confirm.slice(0, confirm.indexOf("\n}\n"));
  if (/:state`|:base`/.test(body))
    fail("10: the confirm path carries state or a modulation base across a renumber " +
         "that no longer destroys anything");
}

if (failures) process.exit(1);
console.log("PASS: chain shape edits — insert, remove and reorder each reach the DSP as ONE " +
            "one-based verb that permutes rather than reloads, no module write goes out for a " +
            "module that only moved, nothing is carried because nothing is destroyed, an " +
            "append still needs no hole, and a saved two-FX slot loads unmigrated");
'
