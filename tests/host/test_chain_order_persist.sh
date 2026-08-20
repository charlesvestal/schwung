#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Pushing a reordered chain to the DSP, and the promise that nothing migrated.
#
# The one this file exists for: SHRINKING must clear EVERY trailing position.
# The natural loop stops at the first blank -- the run is contiguous, so why
# keep walking? -- and that silently leaks every position past the new end.
# Shrink five FX to three and you clear fx4, break, and leave fx5 loaded and
# audible with nothing on screen representing it. The DSP does not save you:
# its fx_count is a high-water mark that only shrinks when the TRAILING slot is
# cleared, so a half-cleared tail stays loaded and stays counted.
#
# Both sections carry the same contract now. The MIDI side used to hide it
# behind an unload-all on slot 1, which was removed because at a cap of 8 it
# destroyed up to seven neighbours.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* Lift a top-level function out of shadow_ui.js and hand it its dependencies
   as parameters. The file cannot be imported -- it is a device UI module full
   of host globals -- but a function that closes over four named things can be
   RUN, which is the difference between pinning the source text and pinning the
   behaviour. */
function lift(name, deps) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
}

const CHAIN_CAP = { midiFx: 8, fx: 8 };

/* A fake slot that behaves like the real one in the way that matters:
   writing <id>:module UNLOADS whatever was there and dlopens a fresh instance
   (v2_load_audio_fx_slot, chain_host.c:217, and v2_load_midi_fx_slot which
   mirrors it), so the old instance state is gone the instant the write lands.
   A fake that merely records writes would let a read-after-write look fine. */
function device(state) {
  const writes = [];
  const reads = [];
  const ops = [];
  const get = (slot, key) => {
    reads.push(key); ops.push("get " + key);
    return state[key] !== undefined ? state[key] : "";
  };
  const set = (slot, key, val) => {
    writes.push(key + "=" + val); ops.push("set " + key);
    const mod = /^(.*):module$/.exec(key);
    if (mod) { state[mod[1] + "_module"] = val; state[mod[1] + ":state"] = ""; }
    else state[key] = val;
    return true;
  };
  return { get, set, writes, reads, ops, state };
}
const dspSection = (prefix, mods) => {
  const o = {};
  o[prefix + "_count"] = String(mods.length);
  mods.forEach((m, i) => { o[prefix + (i + 1) + "_module"] = m; });
  return o;
};

/* Lifted rather than reimplemented: if the read path and the write path ever
   disagree about what position 3 is called, the test must fail with them. */
const makeId = lift("chainSectionId", []);
if (!makeId) process.exit(1);
const chainSectionId = makeId();

const makeWrite = lift("writeChainOrder",
  ["chainConfigs", "CHAIN_CAP", "getSlotParam", "setSlotParam", "chainSectionId"]);
if (!makeWrite) process.exit(1);
const run = (cfg, state) => {
  const d = device(state);
  makeWrite([cfg], CHAIN_CAP, d.get, d.set, chainSectionId)(0);
  return d;
};
const mods = (names) => names.map((n) => (n ? { module: n, params: {} } : null));

/* 1. A REORDER writes both positions, in order, and nothing else. */
{
  const d = run({ midiFx: [], synth: null, fx: mods(["b", "a"]) },
                dspSection("fx", ["a", "b"]));
  if (d.writes.join(" ") !== "fx1:module=b fx2:module=a")
    fail("a reorder should rewrite exactly the two moved positions, got [" + d.writes.join(" ") + "]");
}

/* 2. SHRINKING 5 -> 3 clears fx4 AND fx5. */
{
  const d = run({ midiFx: [], synth: null, fx: mods(["a", "b", "c"]) },
                dspSection("fx", ["a", "b", "c", "d", "e"]));
  if (!d.writes.includes("fx4:module="))
    fail("shrinking left fx4 loaded: [" + d.writes.join(" ") + "]");
  if (!d.writes.includes("fx5:module="))
    fail("shrinking cleared fx4 but LEFT FX5 LOADED AND AUDIBLE: [" + d.writes.join(" ") + "]");
  if (d.writes.length !== 2)
    fail("shrinking touched positions that did not change: [" + d.writes.join(" ") + "]");
}

/* 3. The same for MIDI FX, which has no unload-all covering it any more. */
{
  const d = run({ midiFx: mods(["arp"]), synth: null, fx: [] },
                dspSection("midi_fx", ["arp", "chord", "velocity"]));
  if (!d.writes.includes("midi_fx2:module=") || !d.writes.includes("midi_fx3:module="))
    fail("shrinking the MIDI FX section left a trailing module loaded: [" + d.writes.join(" ") + "]");
}

/* 4. NO WRITE when the order did not change. A redundant write is not free:
      chain_host unloads and reloads the module unconditionally, so rewriting
      the module id a chain already holds destroys its state. */
{
  const d = run({ midiFx: [], synth: null, fx: mods(["a", "b"]) },
                dspSection("fx", ["a", "b"]));
  if (d.writes.length !== 0)
    fail("an unchanged chain was rewritten (each write reloads the module): [" + d.writes.join(" ") + "]");
}

/* 5. And it does not walk the whole cap to find that out. fx_count says where
      the chain ends; probing fx1..fx8 would be eight IPC round trips at ~2.8ms
      to answer a question one read already answered. */
{
  const d = run({ midiFx: [], synth: null, fx: mods(["a", "b"]) },
                dspSection("fx", ["a", "b"]));
  /* Two counts and the two positions those counts vouch for. */
  if (d.reads.length > 4 || d.reads.some((k) => /^(midi_)?fx[3-8]_module$/.test(k)))
    fail("writeChainOrder probed the whole cap: " + d.reads.join(" "));
}

/* ---- carrying each module its own state --------------------------------

   Rewriting a position reloads it from scratch, so a reorder that only moved
   module ids would hand every moved module a default instance: move a tuned
   reverb one place left and you get a factory reverb. The state has to travel
   with it.

   Two rules, and the second is the one that corrupts rather than merely
   disappoints. */
const withState = (prefix, mods) => {
  const o = dspSection(prefix, mods);
  mods.forEach((m, i) => { o[prefix + (i + 1) + ":state"] = "tuned-" + m + "-at" + (i + 1); });
  return o;
};

/* 6. A move carries each moved module ITS OWN state. */
{
  const d = run({ midiFx: [], synth: null, fx: mods(["b", "a"]) },
                withState("fx", ["a", "b"]));
  if (d.state["fx1:state"] !== "tuned-b-at2")
    fail("the module that moved to fx1 did not bring its state: " + d.state["fx1:state"]);
  if (d.state["fx2:state"] !== "tuned-a-at1")
    fail("the module that moved to fx2 did not bring its state: " + d.state["fx2:state"]);
}

/* 7. READ BEFORE WRITE. A module write destroys the instance and its state
      with it, so a state read that happens afterwards returns the fresh
      default and silently succeeds. */
{
  const d = run({ midiFx: [], synth: null, fx: mods(["b", "a"]) },
                withState("fx", ["a", "b"]));
  const firstModuleWrite = d.ops.findIndex((o) => /^set .*:module$/.test(o));
  const lateRead = d.ops.findIndex((o, i) => i > firstModuleWrite && /^get .*:state$/.test(o));
  if (firstModuleWrite >= 0 && lateRead >= 0)
    fail("a state was read AFTER a module write, which reads the fresh default: " + d.ops.join(" | "));
}

/* 8. State is carried ONLY between positions holding the SAME module id.
      A blob is opaque and module-specific: pushing a reverb state into a
      delay is not a lost tweak, it is a corrupted module. Here fx1 is
      REPLACED by a module nothing else holds, while fx2 keeps its own. */
{
  const d = run({ midiFx: [], synth: null, fx: mods(["delay", "b"]) },
                withState("fx", ["reverb", "b"]));
  if (d.writes.some((w) => w.startsWith("fx1:state=")))
    fail("state was pushed into a DIFFERENT module: " + d.writes.join(" "));
  if (d.state["fx2:state"] !== "tuned-b-at2") fail("an untouched position lost its state");
}
/* ...and the shape that actually produces it: a removal shifts the whole
   tail down, so every position ends up holding a different module id than it
   did, one place over. Each must get its own, never its neighbour minus one. */
{
  const d = run({ midiFx: [], synth: null, fx: mods(["delay", "chorus"]) },
                withState("fx", ["reverb", "delay", "chorus"]));
  if (d.state["fx1:state"] !== "tuned-delay-at2")
    fail("after a removal fx1 holds " + d.state["fx1:state"] + " (the reverb state would be corruption)");
  if (d.state["fx2:state"] !== "tuned-chorus-at3")
    fail("after a removal fx2 holds " + d.state["fx2:state"]);
}

/* ...and a chain holding TWO of the same module, which is where matching by
   id alone is not enough. A position that is not moving must claim its own
   state first, or a same-named neighbour that IS moving takes it. Here the
   delay moves left past the second reverb; the first reverb never moves. */
{
  const d = run({ midiFx: [], synth: null, fx: mods(["rev", "delay", "rev"]) },
                withState("fx", ["rev", "rev", "delay"]));
  if (d.state["fx1:state"] !== "tuned-rev-at1")
    fail("the reverb that never moved lost its state: " + d.state["fx1:state"]);
  if (d.state["fx2:state"] !== "tuned-delay-at3")
    fail("the moved delay got " + d.state["fx2:state"]);
  if (d.state["fx3:state"] !== "tuned-rev-at2")
    fail("the second reverb got " + d.state["fx3:state"] + " instead of its own");
}

/* 9. MIDI FX are loaded by v2_load_midi_fx_slot, which mirrors the audio
      loader down to the fresh dlopen, so they have the same defect. */
{
  const d = run({ midiFx: mods(["chord", "arp"]), synth: null, fx: [] },
                withState("midi_fx", ["arp", "chord"]));
  if (d.state["midi_fx1:state"] !== "tuned-chord-at2")
    fail("a moved MIDI FX lost its state: " + d.state["midi_fx1:state"]);
}

/* 10. The IPC stays proportional to what CHANGED, not to the cap: a state is
       read only for a position that is actually receiving a moved module. */
{
  const d = run({ midiFx: [], synth: null, fx: mods(["a", "c", "b"]) },
                withState("fx", ["a", "b", "c"]));
  const stateReads = d.reads.filter((k) => k.endsWith(":state"));
  if (stateReads.length !== 2)
    fail("expected two state reads for the two moved modules, got " + stateReads.join(" "));
  if (d.reads.some((k) => /^fx[4-8]/.test(k)))
    fail("a position past the end of the chain was read: " + d.reads.join(" "));
}

/* 11. THE REGRESSION: an existing saved slot with exactly fx1 and fx2 loads
      with both, in the same order, with the same modules -- and then writing
      that same chain back is a no-op. Nothing about the variable-length chain
      may migrate a patch someone already has. */
{
  const makeLoad = lift("loadChainConfigFromSlot",
    ["chainConfigs", "createEmptyChainConfig", "getSlotParam", "CHAIN_CAP",
     "fxDisplayNameCache", "fxDisplayNameSkip", "fxDisplayNameBackoff"]);
  if (makeLoad) {
    const state = Object.assign({ synth_module: "sf2" },
                                dspSection("fx", ["freeverb", "cloudseed"]));
    const d = device(state);
    const cfgs = [{ midiFx: [], synth: null, fx: [] }];
    const load = makeLoad(cfgs, () => ({ midiFx: [], synth: null, fx: [] }),
                          d.get, CHAIN_CAP, {}, {}, {});
    const cfg = load(0);
    const got = cfg.fx.map((f) => (f ? f.module : "-")).join(",");
    if (got !== "freeverb,cloudseed")
      fail("a saved two-FX slot no longer loads as it was: " + got);
    if (!cfg.synth || cfg.synth.module !== "sf2") fail("the synth did not survive the load");

    const d2 = device(state);
    makeWrite(cfgs, CHAIN_CAP, d2.get, d2.set, chainSectionId)(0);
    if (d2.writes.length !== 0)
      fail("loading a two-FX slot and writing it straight back changed it: [" + d2.writes.join(" ") + "]");
  }
}

if (failures) process.exit(1);
console.log("PASS: chain order persist — reorder writes in order, shrinking clears the WHOLE tail " +
            "(fx and midi fx), unchanged writes nothing, a moved module carries its own state " +
            "read before the first write, and two-FX slots load unmigrated");
'
