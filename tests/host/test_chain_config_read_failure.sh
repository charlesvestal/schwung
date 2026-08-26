#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A failed read must not empty a chain POSITION, and must not latch.
#
# Captured on device 2026-08-26. Loading Osirus into slot 1:
#
#   13:48:53.700 [chain-v2] Loading synth: .../osirus/dsp.so
#   13:48:53.824 [chain-v2] Synth v2 loaded: osirus (16 params)
#   13:49:08.948 Screen reader: "Select Synth, None"      <-- 15s later
#   13:49:11.906 Screen reader: "Slot 1, Synth Virus"     <-- same key, other path
#
# The load was clean and took 124 ms. Fifteen seconds later the chain editor
# still drew the synth position EMPTY -- clicking it opened the module picker,
# which announced the position as None -- while the slot-settings screen, which
# reads the same `synth_module` on a different path, said Virus.
#
# Two causes, and it needs both to become permanent:
#
#   1. loadChainConfigFromSlot's readPosition was `moduleId && moduleId !== ""`,
#      which puts null (the read did not complete) in the same branch as ""
#      (the position is empty). Loading a module blocks the SPI callback -- the
#      thread that also serves param requests -- and applyComponentSelectionConfirmed
#      re-syncs immediately after its fire-and-forget module write, i.e. inside
#      that window. So the position read null and was recorded as empty, and
#      `chainConfigFresh[slot] = true` declared that authoritative.
#
#   2. the module SIGNATURE is a SECOND set of reads, taken milliseconds later.
#      They straddled the end of the load: the config read stale-empty, the
#      signature read the real "osirus". applySlotModuleSignature reloads the
#      config only when the signature CHANGES -- and a signature that is already
#      correct never changes again. The fresh read is what made the stale one
#      permanent.
#
# So both halves are pinned here, and the last case drives the recorded
# sequence end to end.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the chain-config read-failure tests" >&2
  exit 1
fi

node -e '
const fs = require("fs");
const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");

let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };
const check = (name, got, want) => {
  if (JSON.stringify(got) !== JSON.stringify(want))
    fail(`${name}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
  else console.log("ok: " + name);
};

function lift(name, deps) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
}

const CHAIN_CAP = { midiFx: 8, fx: 8 };
const SHADOW_UI_SLOTS = 4;
const createEmptyChainConfig = () => ({ synth: null, midiFx: [], fx: [] });

/* A fake DSP whose answers we choose per key: a string is served, null is a
   read that did not complete. */
function world(answers) {
  const chainConfigs = [];
  const chainConfigFresh = [];
  const reads = [];
  const getSlotParam = (slot, key) => { reads.push(key); return answers(key); };
  const caches = [{}, {}, {}];
  const load = lift("loadChainConfigFromSlot",
    ["chainConfigs", "chainConfigFresh", "getSlotParam", "CHAIN_CAP",
     "createEmptyChainConfig", "fxDisplayNameCache", "fxDisplayNameSkip",
     "fxDisplayNameBackoff"])(
    chainConfigs, chainConfigFresh, getSlotParam, CHAIN_CAP,
    createEmptyChainConfig, caches[0], caches[1], caches[2]);
  const sig = lift("getSlotModuleSignature", ["getSlotParam", "CHAIN_CAP"])(
    getSlotParam, CHAIN_CAP);
  return { chainConfigs, chainConfigFresh, reads, load, sig };
}

/* Answer tables. "" is served-and-empty; null is a read that failed. */
const allNull  = () => null;
const allEmpty = () => "";
const loaded   = (k) => (k === "synth_module" ? "osirus"
                       : k === "fx_count" || k === "midi_fx_count" ? "0" : "");

/* ---- 1. a failed read keeps the position ------------------------------ */
{
  const w = world(allNull);
  w.chainConfigs[0] = { synth: { module: "osirus", params: {} }, midiFx: [], fx: [] };
  const cfg = w.load(0);
  check("a failed read keeps the module in the position",
        cfg.synth && cfg.synth.module, "osirus");
  check("a failed read leaves the slot NOT fresh, so the next frame re-reads",
        w.chainConfigFresh[0], false);
}

/* ---- 2. served-and-empty still empties it ----------------------------- */
{
  const w = world(allEmpty);
  w.chainConfigs[0] = { synth: { module: "osirus", params: {} }, midiFx: [], fx: [] };
  const cfg = w.load(0);
  check("\"\" still means the position is empty", cfg.synth, null);
  check("a clean read latches the slot fresh", w.chainConfigFresh[0], true);
}

/* ---- 3. a served module lands, and latches ---------------------------- */
{
  const w = world(loaded);
  const cfg = w.load(0);
  check("a served module lands in the position", cfg.synth.module, "osirus");
  check("a served read latches the slot fresh", w.chainConfigFresh[0], true);
}

/* ---- 4. a failed COUNT must not truncate the section ------------------ */
{
  const w = world((k) => (k === "fx_count" ? null : k === "midi_fx_count" ? "0"
                        : k === "synth_module" ? "osirus" : null));
  w.chainConfigs[0] = { synth: null, midiFx: [],
                        fx: [{ module: "freeverb", params: {} }] };
  const cfg = w.load(0);
  check("a failed fx_count keeps the section rather than emptying it",
        cfg.fx.length && cfg.fx[0].module, "freeverb");
}

/* ---- 5. the signature refuses to answer from a failed read ------------ */
{
  check("a signature built from a failed read is null", world(allNull).sig(0), null);
  const good = world(loaded).sig(0);
  if (typeof good !== "string" || !good.includes("osirus"))
    fail(`a clean signature should be a string naming the module, got ${JSON.stringify(good)}`);
  else console.log("ok: a clean read still produces a real signature");
}

/* ---- 6. and a null signature is never latched ------------------------- */
{
  const last = ["", "", "", ""];
  let reloads = 0;
  const apply = lift("applySlotModuleSignature",
    ["SHADOW_UI_SLOTS", "lastSlotModuleSignatures", "loadChainConfigFromSlot",
     "invalidateKnobContextCache", "needsRedraw"])(
    SHADOW_UI_SLOTS, last, () => { reloads++; }, () => {}, false);
  check("a null signature is not news", apply(0, null), false);
  check("...and is not latched", last[0], "");
  check("a real signature still reloads", apply(0, "osirus|/"), true);
  check("...and is latched", last[0], "osirus|/");
  check("the same signature twice reloads once", apply(0, "osirus|/"), false);
  check("one reload, not two", reloads, 1);
}

/* ---- 7. THE RECORDED SEQUENCE, end to end ---------------------------- *
 *
 * The picker puts the chosen module into chainConfigs BEFORE writing it to the
 * DSP (applyChainComponentPick), then applyComponentSelectionConfirmed
 * re-syncs. Frame 1 is inside the load: every read fails. Frame 2 is after it:
 * every read lands.
 *
 * The position must be drawn as Osirus THROUGHOUT -- never as the empty box
 * whose click opens the module picker.
 */
{
  let inLoad = true;
  const w = world((k) => (inLoad ? null : loaded(k)));
  /* what the picker left behind */
  w.chainConfigs[0] = { synth: { module: "osirus", params: {} }, midiFx: [], fx: [] };

  const f1 = w.load(0);                       /* the re-sync, mid-load */
  check("frame 1: the position is not emptied by the load it is racing",
        f1.synth && f1.synth.module, "osirus");
  const sigDuring = w.sig(0);
  check("frame 1: the signature declines to answer", sigDuring, null);
  if (w.chainConfigFresh[0] !== false)
    fail("frame 1: the slot was latched fresh, so nothing would ever re-read it");

  inLoad = false;
  const f2 = w.load(0);                       /* next frame, load finished */
  check("frame 2: the position still reads Osirus",
        f2.synth && f2.synth.module, "osirus");
  check("frame 2: the slot is clean now", w.chainConfigFresh[0], true);
}

if (failures) { console.log(`FAILED: ${failures} check(s)`); process.exit(1); }
console.log("PASS: chain config read failure — a timeout empties no position and latches nothing");
'
