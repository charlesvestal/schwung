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
# The second: a position that changes is RELOADED from scratch, so what moves
# has to carry its state, its modulation routing, its modulation BASE -- which
# lives in the chain host modulation table, not in the state blob, and is
# cleared by the unload -- and nothing belonging to anybody else. Every fixture below expresses a reorder the way the model
# expresses one -- the SAME module objects in a different order -- because
# identity is the only thing that can tell two instances of one module apart.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { parseId as parseChainId } from "./src/shared/chain_model.mjs";
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
    if (mod) {
      state[mod[1] + "_module"] = val; state[mod[1] + ":state"] = "";
      /* Unloading a position ALSO drops its modulation table entries
         (chain_mod_clear_target_entries, chain_host.c) -- and a modulated
         param base lives only there, so it goes with them. Modelling that is
         the whole point: without it a base read after the write would answer
         the old value and a broken carry would look like it worked. */
      for (const k of Object.keys(state)) {
        if (k.indexOf(mod[1] + ":") === 0 && /:base$/.test(k)) delete state[k];
      }
    }
    else state[key] = val;
    return true;
  };
  return { get, set, writes, reads, ops, state };
}
const dspSection = (prefix, names) => {
  const o = {};
  o[prefix + "_count"] = String(names.length);
  names.forEach((m, i) => { o[prefix + (i + 1) + "_module"] = m; });
  return o;
};
/* Distinguishable per-POSITION state, so a carry that lands one place off, or
   takes a same-named neighbour blob, is visible rather than plausible. */
const withState = (prefix, names) => {
  const o = dspSection(prefix, names);
  names.forEach((m, i) => { o[prefix + (i + 1) + ":state"] = "tuned-" + m + "-at" + (i + 1); });
  return o;
};

/* Lifted rather than reimplemented: if the read path and the write path ever
   disagree about what position 3 is called, the test must fail with them. */
const makeId = lift("chainSectionId", []);
if (!makeId) process.exit(1);
const chainSectionId = makeId();

/* invalidateChainConfig: writeChainOrder also drops the editor cached view of
   the slot, because it renumbers positions and rewrites state and LFO routing.
   Stubbed here -- what it does is pinned in test_chain_edit_read_budget.sh. */
const makeWrite = lift("writeChainOrder",
  ["chainConfigs", "CHAIN_CAP", "getSlotParam", "setSlotParam", "chainSectionId",
   "parseChainId", "invalidateChainConfig"]);
if (!makeWrite) process.exit(1);

/*
 * `prev` is the section as it was BEFORE the edit, and it matters that its
 * entries are the SAME OBJECTS the new list holds. The chain model shares
 * module objects across its clones, so identity is what says "this position
 * used to be that one" -- and identity is the only thing that CAN say it when
 * two positions hold the same module id.
 */
const run = (prev, now, state, section) => {
  const sec = section || "fx";
  const cfg = { midiFx: [], synth: null, fx: [] };
  cfg[sec] = now;
  const d = device(state);
  makeWrite([cfg], CHAIN_CAP, d.get, d.set, chainSectionId, parseChainId, () => {})(0, sec, prev);
  return d;
};
const mods = (names) => names.map((n) => (n ? { module: n, params: {} } : null));
const pick = (list, order) => order.map((i) => list[i]);

/* 1. A REORDER of two different modules writes both positions and nothing
      else -- their ids changed, so both instances must be rebuilt. */
{
  const prev = mods(["a", "b"]);
  const d = run(prev, pick(prev, [1, 0]), dspSection("fx", ["a", "b"]));
  const moduleWrites = d.writes.filter((w) => w.includes(":module="));
  if (moduleWrites.join(" ") !== "fx1:module=b fx2:module=a")
    fail("a reorder should rewrite exactly the two moved positions, got [" + moduleWrites.join(" ") + "]");
}

/* 2. SHRINKING 5 -> 3 clears fx4 AND fx5. */
{
  const prev = mods(["a", "b", "c", "d", "e"]);
  const d = run(prev, prev.slice(0, 3), dspSection("fx", ["a", "b", "c", "d", "e"]));
  if (!d.writes.includes("fx4:module="))
    fail("shrinking left fx4 loaded: [" + d.writes.join(" ") + "]");
  if (!d.writes.includes("fx5:module="))
    fail("shrinking cleared fx4 but LEFT FX5 LOADED AND AUDIBLE: [" + d.writes.join(" ") + "]");
  if (d.writes.length !== 2)
    fail("shrinking touched positions that did not change: [" + d.writes.join(" ") + "]");
}

/* 3. The same for MIDI FX, which has no unload-all covering it any more. */
{
  const prev = mods(["arp", "chord", "velocity"]);
  const d = run(prev, prev.slice(0, 1),
                dspSection("midi_fx", ["arp", "chord", "velocity"]), "midiFx");
  if (!d.writes.includes("midi_fx2:module=") || !d.writes.includes("midi_fx3:module="))
    fail("shrinking the MIDI FX section left a trailing module loaded: [" + d.writes.join(" ") + "]");
}

/* 4. NO WRITE when nothing changed. A redundant module write is not free:
      chain_host unloads and reloads unconditionally, so rewriting the id a
      chain already holds destroys that module state. */
{
  const prev = mods(["a", "b"]);
  const d = run(prev, prev.slice(), dspSection("fx", ["a", "b"]));
  if (d.writes.length !== 0)
    fail("an unchanged chain was rewritten (each write reloads the module): [" + d.writes.join(" ") + "]");
}

/* 5. And it does not walk the whole cap to find that out. fx_count says where
      the chain ends; probing fx1..fx8 would be eight IPC round trips at ~2.8ms
      to answer a question one read already answered. */
{
  const prev = mods(["a", "b"]);
  const d = run(prev, prev.slice(), dspSection("fx", ["a", "b"]));
  if (d.reads.length > 3 || d.reads.some((k) => /^(midi_)?fx[3-8]_module$/.test(k)))
    fail("writeChainOrder probed the whole cap: " + d.reads.join(" "));
}

/* ---- carrying each module its own state --------------------------------

   A position that changes is reloaded from scratch, so a reorder that moved
   only module ids would hand every moved module a factory instance: move a
   tuned reverb one place left and you get a default reverb. */

/* 6. A move carries each moved module ITS OWN state. */
{
  const prev = mods(["a", "b"]);
  const d = run(prev, pick(prev, [1, 0]), withState("fx", ["a", "b"]));
  if (d.state["fx1:state"] !== "tuned-b-at2")
    fail("the module that moved to fx1 did not bring its state: " + d.state["fx1:state"]);
  if (d.state["fx2:state"] !== "tuned-a-at1")
    fail("the module that moved to fx2 did not bring its state: " + d.state["fx2:state"]);
}

/* 7. READ BEFORE WRITE. A module write destroys the instance and its state
      with it, so a state read that happens afterwards returns the fresh
      default and silently succeeds. */
{
  const prev = mods(["a", "b"]);
  const d = run(prev, pick(prev, [1, 0]), withState("fx", ["a", "b"]));
  const firstModuleWrite = d.ops.findIndex((o) => /^set .*:module$/.test(o));
  const lateRead = d.ops.findIndex((o, i) => i > firstModuleWrite && /^get .*:state$/.test(o));
  if (firstModuleWrite >= 0 && lateRead >= 0)
    fail("a state was read AFTER a module write, which reads the fresh default: " + d.ops.join(" | "));
}

/* 8. State follows the MODULE, never the position. A blob is opaque and
      module-specific: pushing a reverb state into a delay is not a lost tweak,
      it is a corrupted module. Here fx1 is REPLACED by a module that was not
      in the chain, so it must start clean. */
{
  const prev = mods(["reverb", "b"]);
  const d = run(prev, [{ module: "delay", params: {} }, prev[1]],
                withState("fx", ["reverb", "b"]));
  if (d.writes.some((w) => w.startsWith("fx1:state=")))
    fail("state was pushed into a DIFFERENT module: " + d.writes.join(" "));
  if (d.state["fx2:state"] !== "tuned-b-at2") fail("an untouched position lost its state");
}
/* ...and the shape that actually produces it: a removal shifts the whole tail
   down, so every position ends up holding a different module than it did, one
   place over. Each must get its own, never its neighbour minus one. */
{
  const prev = mods(["reverb", "delay", "chorus"]);
  const d = run(prev, prev.slice(1), withState("fx", ["reverb", "delay", "chorus"]));
  if (d.state["fx1:state"] !== "tuned-delay-at2")
    fail("after a removal fx1 holds " + d.state["fx1:state"] + " (the reverb state would be corruption)");
  if (d.state["fx2:state"] !== "tuned-chorus-at3")
    fail("after a removal fx2 holds " + d.state["fx2:state"]);
}

/* 9. TWO INSTANCES OF THE SAME MODULE, reordered.
      This is the case a name comparison cannot see at all: every position
      still reports the same module id, so a writer that decides by id issues
      no writes, each instance keeps the state of the position it used to be
      in, and the editor announces a move that did not happen. Identity is the
      only thing that distinguishes them.
      Note what SHOULD happen: no module writes at all -- neither instance
      needs reloading, they are the same plugin -- and the two states swap. */
{
  const prev = mods(["rev", "rev"]);
  const d = run(prev, pick(prev, [1, 0]), withState("fx", ["rev", "rev"]));
  if (d.writes.some((w) => w.includes(":module=")))
    fail("reordering two of the same module reloaded them for nothing: [" + d.writes.join(" ") + "]");
  if (d.state["fx1:state"] !== "tuned-rev-at2" || d.state["fx2:state"] !== "tuned-rev-at1")
    fail("reordering two of the same module was a SILENT NO-OP: fx1=" +
         d.state["fx1:state"] + " fx2=" + d.state["fx2:state"]);
}
/* ...and one that stays put while a same-named sibling moves past it keeps
   its own state and is not written at all. */
{
  const prev = mods(["rev", "rev", "delay"]);
  const d = run(prev, pick(prev, [0, 2, 1]), withState("fx", ["rev", "rev", "delay"]));
  if (d.writes.some((w) => w.startsWith("fx1:")))
    fail("a module that never moved was written: [" + d.writes.join(" ") + "]");
  if (d.state["fx1:state"] !== "tuned-rev-at1")
    fail("the reverb that never moved lost its state: " + d.state["fx1:state"]);
  if (d.state["fx2:state"] !== "tuned-delay-at3")
    fail("the moved delay got " + d.state["fx2:state"]);
  if (d.state["fx3:state"] !== "tuned-rev-at2")
    fail("the second reverb got " + d.state["fx3:state"] + " instead of its own");
}

/* 10. MIDI FX are loaded by v2_load_midi_fx_slot, which mirrors the audio
       loader down to the fresh dlopen, so they have the same defect. */
{
  const prev = mods(["arp", "chord"]);
  const d = run(prev, pick(prev, [1, 0]), withState("midi_fx", ["arp", "chord"]), "midiFx");
  if (d.state["midi_fx1:state"] !== "tuned-chord-at2")
    fail("a moved MIDI FX lost its state: " + d.state["midi_fx1:state"]);
}

/* 11. The IPC stays proportional to what CHANGED, not to the cap: a state is
       read only for a position actually receiving a moved module. */
{
  const prev = mods(["a", "b", "c"]);
  const d = run(prev, pick(prev, [0, 2, 1]), withState("fx", ["a", "b", "c"]));
  const stateReads = d.reads.filter((k) => k.endsWith(":state"));
  if (stateReads.length !== 2)
    fail("expected two state reads for the two moved modules, got " + stateReads.join(" "));
  if (d.reads.some((k) => /^fx[4-8]/.test(k)))
    fail("a position past the end of the chain was read: " + d.reads.join(" "));
}

/* ---- modulation follows the module too ---------------------------------

   An LFO routes by POSITION key ("fx2"), which was safe only while positions
   never moved. Left alone, a reorder re-points every LFO at whatever module
   slid into the slot it was aimed at -- the chain sounds different afterwards
   in a way the user did not ask for and cannot see. */

/* 12. A reorder carries the routing with the module. */
{
  const prev = mods(["a", "b"]);
  const state = Object.assign(withState("fx", ["a", "b"]),
                              { "lfo1:target": "fx1", "lfo2:target": "synth" });
  const d = run(prev, pick(prev, [1, 0]), state);
  if (d.state["lfo1:target"] !== "fx2")
    fail("an LFO aimed at the module that moved to fx2 still says " + d.state["lfo1:target"]);
  if (d.writes.some((w) => w.startsWith("lfo2:target=")))
    fail("an LFO aimed at the synth was rewritten by an FX reorder");
}
/* 13. A removal takes its routing with it rather than silently re-aiming the
       LFO at the module that moved up into the empty slot. */
{
  const prev = mods(["a", "b", "c"]);
  const state = Object.assign(withState("fx", ["a", "b", "c"]), { "lfo1:target": "fx2" });
  const d = run(prev, [prev[0], prev[2]], state);
  if (d.state["lfo1:target"] !== "")
    fail("an LFO aimed at a REMOVED module now points at " + d.state["lfo1:target"]);
}
/* 14. ...and an LFO aimed at something that did not move is left alone, so
       the remap is not just clearing everything it touches. */
{
  const prev = mods(["a", "b", "c"]);
  const state = Object.assign(withState("fx", ["a", "b", "c"]), { "lfo1:target": "fx1" });
  const d = run(prev, pick(prev, [0, 2, 1]), state);
  if (d.writes.some((w) => w.startsWith("lfo1:target=")))
    fail("an LFO aimed at a module that never moved was rewritten");
}
/* 14b. A CLEARED routing clears both halves. `target_param` left behind names
        a param of a module that is no longer anywhere in the chain, and the
        next write of `target` alone revives that half of the routing. */
{
  const prev = mods(["a", "b", "c"]);
  const state = Object.assign(withState("fx", ["a", "b", "c"]),
                              { "lfo1:target": "fx2", "lfo1:target_param": "room_size" });
  const d = run(prev, [prev[0], prev[2]], state);
  if (d.state["lfo1:target_param"] !== "")
    fail("a cleared LFO routing still names " + d.state["lfo1:target_param"] +
         ", a param of the module that just left the chain");
}
/* 14c. ...but a REMAPPED routing keeps its param: the module is still there,
        one place over, and the param it was driving is still that module. */
{
  const prev = mods(["a", "b"]);
  const state = Object.assign(withState("fx", ["a", "b"]),
                              { "lfo1:target": "fx1", "lfo1:target_param": "volume" });
  const d = run(prev, pick(prev, [1, 0]), state);
  if (d.state["lfo1:target_param"] !== "volume")
    fail("a remapped LFO lost its param: " + d.state["lfo1:target_param"]);
}

/* ---- carrying the modulation BASE --------------------------------------

   A modulated param base does not live in the module opaque `:state` blob. It
   lives in the chain host modulation table (mod_target_state_t.base_value),
   keyed by the POSITION id, and unloading the position clears it. So a reverb
   set to 50% and then modulated came back at 100% after a reorder: the LFO
   followed the module, the base did not.

   `withMod` gives the two readings the DSP actually serves for a modulated
   param, and makes them DIFFER -- the plain key answers the EFFECTIVE value
   (wherever the LFO has pushed it this instant) and only `:base` answers the
   base. Carrying the effective value would be a subtle, silent corruption. */
const withMod = (o, id, param, base, effective) => {
  o[id + ":" + param + ":base"] = base;
  o[id + ":" + param] = effective;
  return o;
};

/* 15. THE BUG: reorder a modulated module and its base moves with it. */
{
  const prev = mods(["psxverb", "b"]);
  const state = withMod(Object.assign(withState("fx", ["psxverb", "b"]),
                        { "lfo1:target": "fx1", "lfo1:target_param": "volume" }),
                        "fx1", "volume", "0.5", "0.87");
  const d = run(prev, pick(prev, [1, 0]), state);
  if (d.state["fx2:volume"] !== "0.5")
    fail("a modulated module moved to fx2 and its base did not follow: fx2:volume=" +
         d.state["fx2:volume"] + " (the reported bug: 50% came back as the default)");
}
/* 16. ...and it is the BASE that travels, never the momentary modulated
       value the plain key answers. */
{
  const prev = mods(["psxverb", "b"]);
  const state = withMod(Object.assign(withState("fx", ["psxverb", "b"]),
                        { "lfo1:target": "fx1", "lfo1:target_param": "volume" }),
                        "fx1", "volume", "0.5", "0.87");
  const d = run(prev, pick(prev, [1, 0]), state);
  if (d.state["fx2:volume"] === "0.87")
    fail("the LFO instantaneous value was carried in as the new base");
  if (!d.reads.includes("fx1:volume:base"))
    fail("the base was never read from the position the module came from: " + d.reads.join(" "));
  if (d.reads.includes("fx1:volume"))
    fail("the plain key was read, which answers the EFFECTIVE value while a target is active");
}
/* 17. READ BEFORE WRITE, for the base too. The module write drops the
       modulation entry, so a base read afterwards answers nothing at all and
       the carry silently does nothing. */
{
  const prev = mods(["psxverb", "b"]);
  const state = withMod(Object.assign(withState("fx", ["psxverb", "b"]),
                        { "lfo1:target": "fx1", "lfo1:target_param": "volume" }),
                        "fx1", "volume", "0.5", "0.87");
  const d = run(prev, pick(prev, [1, 0]), state);
  const firstModuleWrite = d.ops.findIndex((o) => /^set .*:module$/.test(o));
  const lateRead = d.ops.findIndex((o, i) => i > firstModuleWrite && /^get .*:base$/.test(o));
  if (firstModuleWrite >= 0 && lateRead >= 0)
    fail("a base was read AFTER a module write, which reads nothing: " + d.ops.join(" | "));
}
/* 18. The base is written AFTER the state, because the state blob was
       captured while the param was modulated and carries the effective value
       -- written second it would overwrite the base back to the wrong one. */
{
  const prev = mods(["psxverb", "b"]);
  const state = withMod(Object.assign(withState("fx", ["psxverb", "b"]),
                        { "lfo1:target": "fx1", "lfo1:target_param": "volume" }),
                        "fx1", "volume", "0.5", "0.87");
  const d = run(prev, pick(prev, [1, 0]), state);
  const stateWrite = d.ops.lastIndexOf("set fx2:state");
  const baseWrite = d.ops.indexOf("set fx2:volume");
  if (baseWrite < 0) fail("no base write at all");
  else if (stateWrite >= 0 && baseWrite < stateWrite)
    fail("the base was written before the state, so the state overwrote it: " + d.ops.join(" | "));
}
/* 19. And the routing moves BEFORE the state. Setting lfoN:target makes the
       DSP drop the old source, which RESTORES that source base onto the
       position it was aimed at -- after the state writes, that restore lands
       on top of freshly carried state. Visible when two of the SAME module
       swap, which issues no module writes and so leaves both live. */
{
  const prev = mods(["psxverb", "b"]);
  const state = withMod(Object.assign(withState("fx", ["psxverb", "b"]),
                        { "lfo1:target": "fx1", "lfo1:target_param": "volume" }),
                        "fx1", "volume", "0.5", "0.87");
  const d = run(prev, pick(prev, [1, 0]), state);
  const routing = d.ops.indexOf("set lfo1:target");
  const firstState = d.ops.findIndex((o) => /^set .*:state$/.test(o));
  if (routing < 0) fail("the routing was never rewritten");
  else if (firstState >= 0 && routing > firstState)
    fail("the routing was rewritten after a state write, so the DSP base restore " +
         "lands on carried state: " + d.ops.join(" | "));
}
/* 20. A base is carried by IDENTITY, exactly as state is. A picker swap puts
       a module that was never in the chain at fx1: the old base is meaningless
       to it and must not be inherited. */
{
  const prev = mods(["psxverb", "b"]);
  const state = withMod(Object.assign(withState("fx", ["psxverb", "b"]),
                        { "lfo1:target": "fx1", "lfo1:target_param": "volume" }),
                        "fx1", "volume", "0.5", "0.87");
  const d = run(prev, [{ module: "delay", params: {} }, prev[1]], state);
  if (d.writes.some((w) => /^fx\d+:volume=/.test(w)))
    fail("a base was pushed into a module that was never in the chain: " + d.writes.join(" "));
  if (d.state["lfo1:target"] !== "")
    fail("an LFO aimed at a module replaced by a picker swap still points at " +
         d.state["lfo1:target"]);
}
/* 21. MIDI FX unload through the same clear (chain_midi.c mirrors the audio
       path), so they lose the base the same way and carry it the same way. */
{
  const prev = mods(["arp", "chord"]);
  const state = withMod(Object.assign(withState("midi_fx", ["arp", "chord"]),
                        { "lfo1:target": "midi_fx1", "lfo1:target_param": "rate" }),
                        "midi_fx1", "rate", "0.25", "0.61");
  const d = run(prev, pick(prev, [1, 0]), state, "midiFx");
  if (d.state["midi_fx2:rate"] !== "0.25")
    fail("a moved MIDI FX lost its modulation base: " + d.state["midi_fx2:rate"]);
}
/* 22. The extra IPC is bounded by the LFOs that actually name a moved module,
       not by the cap. Nothing moved here, so nothing is read. */
{
  const prev = mods(["a", "b"]);
  const state = Object.assign(withState("fx", ["a", "b"]),
                              { "lfo1:target": "fx1", "lfo1:target_param": "volume" });
  const d = run(prev, prev.slice(), state);
  if (d.reads.some((k) => /:base$/.test(k)))
    fail("a base was read although nothing moved: " + d.reads.join(" "));
}
/* 23. ...and an LFO with no param stored asks for no base -- `<id>::base` is
       not a key, and reading it would be a wasted round trip per gesture. */
{
  const prev = mods(["a", "b"]);
  const state = Object.assign(withState("fx", ["a", "b"]), { "lfo1:target": "fx1" });
  const d = run(prev, pick(prev, [1, 0]), state);
  if (d.reads.some((k) => /:base$/.test(k)))
    fail("a base was read for an LFO with no target param: " + d.reads.join(" "));
}
/* 24. An unserved `:base` answers "" (the shim zeroes the buffer on error),
       which is a MISS. Writing it in would be the same corruption in the other
       direction -- 100% turning into 0%. */
{
  const prev = mods(["a", "b"]);
  const state = Object.assign(withState("fx", ["a", "b"]),
                              { "lfo1:target": "fx1", "lfo1:target_param": "volume" });
  const d = run(prev, pick(prev, [1, 0]), state);
  if (d.writes.some((w) => w.startsWith("fx2:volume=")))
    fail("an empty base read was written in as a value: " + d.writes.join(" "));
}

/* 25. THE REGRESSION: an existing saved slot with exactly fx1 and fx2 loads
       with both, in the same order, with the same modules -- and then writing
       that same chain back is a no-op. Nothing about the variable-length chain
       may migrate a patch someone already has. */
{
  const makeLoad = lift("loadChainConfigFromSlot",
    ["chainConfigs", "createEmptyChainConfig", "getSlotParam", "CHAIN_CAP",
     "fxDisplayNameCache", "fxDisplayNameSkip", "fxDisplayNameBackoff", "chainConfigFresh"]);
  if (makeLoad) {
    const state = Object.assign({ synth_module: "sf2" },
                                dspSection("fx", ["freeverb", "cloudseed"]));
    const d = device(state);
    const cfgs = [{ midiFx: [], synth: null, fx: [] }];
    const load = makeLoad(cfgs, () => ({ midiFx: [], synth: null, fx: [] }),
                          d.get, CHAIN_CAP, {}, {}, {}, []);
    const cfg = load(0);
    const got = cfg.fx.map((f) => (f ? f.module : "-")).join(",");
    if (got !== "freeverb,cloudseed")
      fail("a saved two-FX slot no longer loads as it was: " + got);
    if (!cfg.synth || cfg.synth.module !== "sf2") fail("the synth did not survive the load");

    const d2 = device(state);
    makeWrite(cfgs, CHAIN_CAP, d2.get, d2.set, chainSectionId, parseChainId, () => {})(
      0, "fx", cfg.fx.slice());
    if (d2.writes.length !== 0)
      fail("loading a two-FX slot and writing it straight back changed it: [" + d2.writes.join(" ") + "]");
  }
}

if (failures) process.exit(1);
console.log("PASS: chain order persist — shrinking clears the WHOLE tail (fx and midi fx), " +
            "unchanged writes nothing, a moved module carries its state, its modulation " +
            "routing AND its modulation BASE (read as :base, never the modulated value) by " +
            "identity, a cleared routing clears target_param too, read before the first " +
            "write, and two-FX slots load unmigrated");
'
