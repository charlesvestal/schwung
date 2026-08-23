#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The loaded-preset record rides the slot autosave, not a param or SHM field --
# it is pure UI bookkeeping the DSP never sees (see current_preset.mjs). This
# pins two things: buildSlotPatchJson writes {name, hash} from INSIDE
# componentEntry, conditionally, threaded through all three chain-position
# shapes (synth/config, midi_fx/params, audio_fx/params); and the boot-restore
# loop reads it back per component prefix, clearing any stale record when a
# patch carries none -- which is every patch written before this existed.
#
# The BAIL guard (stateJson === null && bailIfEmpty -> abandon the whole save)
# must be untouched: it still has to fire before any user_preset write, or a
# timed-out state read would get silently papered over by a preset record.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";

const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* Lift a top-level function out of shadow_ui.js and hand it its dependencies
   as parameters -- the file cannot be imported (host globals everywhere) but
   a function closing over a few named things can be RUN. Same technique as
   test_chain_edit_read_budget.sh. */
function lift(name, deps) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
}

/* ---- structural: the record store exists and is exactly two functions ---- */
if (!/const currentUserPresets = Object\.create\(null\)/.test(src)) {
  fail("no currentUserPresets store -- the record has nowhere to live between save and load");
}
if (!/function getUserPresetRecord\(/.test(src) || !/function setUserPresetRecord\(/.test(src)) {
  fail("getUserPresetRecord/setUserPresetRecord missing -- write side and read side need a shared accessor");
}

/* ---- structural: BAIL guard is untouched -- still returns BAIL before any
   user_preset write, on the RAW null (not the collapsed falsy) check ------- */
{
  const at = src.indexOf("function buildSlotPatchJson(");
  const end = src.indexOf("\n}\n", at);
  const body = src.slice(at, end);
  const bailAt = body.indexOf("stateJson === null && bailIfEmpty");
  const returnBailAt = body.indexOf("return BAIL;");
  const userPresetAt = body.indexOf("entry.user_preset = ");
  if (bailAt < 0 || returnBailAt < 0) {
    fail("BAIL guard (stateJson === null && bailIfEmpty -> return BAIL) is gone");
  } else if (!(bailAt < returnBailAt && returnBailAt < userPresetAt)) {
    fail("BAIL must still return before any user_preset bookkeeping runs");
  }
}

/* ---- structural: all three consumer shapes carry user_preset through ---- */
{
  const at = src.indexOf("function buildSlotPatchJson(");
  const end = src.indexOf("\n}\n", at);
  const body = src.slice(at, end);
  const synthAt = body.indexOf("patch.synth = {");
  const midiAt = body.indexOf("patch.midi_fx.push({");
  const fxAt = body.indexOf("patch.audio_fx.push({");
  for (const [label, at2] of [["synth", synthAt], ["midi_fx", midiAt], ["audio_fx", fxAt]]) {
    if (at2 < 0) { fail(label + " consumer block not found"); continue; }
    const block = body.slice(at2, body.indexOf("}", body.indexOf("{", at2) + 200));
    const chunk = body.slice(at2, at2 + 260);
    if (!/user_preset:\s*entry\.user_preset/.test(chunk)) {
      fail(label + " consumer does not carry entry.user_preset through -- a preset mark on this " +
           "chain position would silently never persist");
    }
  }
}

/* ---- behavioural: run buildSlotPatchJson for real ----------------------- */
const deps = ["chainConfigs", "getSlotStateWithRetry", "debugLog", "getSlotParam", "getUserPresetRecord"];
const buildSlotPatchJsonMaker = lift("buildSlotPatchJson", deps);

if (buildSlotPatchJsonMaker) {
  const cfg = {
    synth: { module: "granny", params: {} },
    midiFx: [{ module: "chord", params: {} }],
    fx: [{ module: "freeverb", params: {} }]
  };
  const chainConfigs = { 0: cfg };
  const state = { "synth:state": "SYNTH_BLOB", "midi_fx1:state": "MFX_BLOB", "fx1:state": "FX_BLOB" };
  const getSlotStateWithRetry = (slot, key) => (key in state ? state[key] : "");
  const getSlotParam = (slot, key) => "0";
  const debugLog = () => {};

  /* Case A: nothing loaded a preset anywhere in this slot -- absent means
     absent, on every one of the three shapes. */
  const noRecord = () => null;
  const buildNone = buildSlotPatchJsonMaker(chainConfigs, getSlotStateWithRetry, debugLog, getSlotParam, noRecord);
  const patchNone = JSON.parse(buildNone(0, "Name", false, false));
  if ("user_preset" in patchNone.synth) fail("no preset loaded, but synth carries a user_preset key");
  if ("user_preset" in patchNone.midi_fx[0]) fail("no preset loaded, but midi_fx carries a user_preset key");
  if ("user_preset" in patchNone.audio_fx[0]) fail("no preset loaded, but audio_fx carries a user_preset key");

  /* Case B: a preset is loaded on the synth only -- the OTHER two positions
     must not pick it up (keyed by slot+prefix, not by slot alone). */
  const onlySynth = (slot, prefix) => (prefix === "synth" ? { name: "Fat Brass", hash: "abc:3" } : null);
  const buildSynth = buildSlotPatchJsonMaker(chainConfigs, getSlotStateWithRetry, debugLog, getSlotParam, onlySynth);
  const patchSynth = JSON.parse(buildSynth(0, "Name", false, false));
  if (!patchSynth.synth.user_preset || patchSynth.synth.user_preset.name !== "Fat Brass") {
    fail("synth preset record did not reach the saved patch");
  }
  if (patchSynth.synth.user_preset.hash !== "abc:3") fail("synth preset hash did not reach the saved patch");
  if ("user_preset" in patchSynth.midi_fx[0]) fail("midi_fx picked up the synth preset record -- keys are not per-prefix");
  if ("user_preset" in patchSynth.audio_fx[0]) fail("audio_fx picked up the synth preset record -- keys are not per-prefix");

  /* Case C: a preset on the audio FX only. */
  const onlyFx = (slot, prefix) => (prefix === "fx1" ? { name: "Warm Plate", hash: "def:5" } : null);
  const buildFx = buildSlotPatchJsonMaker(chainConfigs, getSlotStateWithRetry, debugLog, getSlotParam, onlyFx);
  const patchFx = JSON.parse(buildFx(0, "Name", false, false));
  if (!patchFx.audio_fx[0].user_preset || patchFx.audio_fx[0].user_preset.name !== "Warm Plate") {
    fail("audio_fx preset record did not reach the saved patch");
  }
  if ("user_preset" in patchFx.synth) fail("synth picked up the fx preset record");

  /* Case D: the BAIL guard still fires and abandons the WHOLE save -- a
     record must not paper over a failed state read. */
  const failingState = (slot, key) => (key === "synth:state" ? null : "");
  const buildBailed = buildSlotPatchJsonMaker(chainConfigs, failingState, debugLog, getSlotParam,
    () => ({ name: "Should Not Matter", hash: "x:1" }));
  const bailed = buildBailed(0, "Name", true, false);
  if (bailed !== null) fail("a failed state read with bailIfEmpty must still abandon the whole save (got non-null)");
}

/* ---- behavioural: the reader (entryUserPreset) ---------------------------
   Same technique -- lift the pure extractor used by the boot-restore loop. */
{
  const at = src.indexOf("function entryUserPreset(");
  const end = src.indexOf("\n}\n", at);
  if (at < 0 || end < 0) {
    fail("entryUserPreset (the boot-restore reader) is gone -- the record would be write-only");
  } else {
    const entryUserPreset = new Function(src.slice(at, end + 2) + "\nreturn entryUserPreset;")();

    /* A patch written before user_preset existed: no key at all. */
    if (entryUserPreset({ module: "granny", config: {}, bypassed: 0 }) !== null) {
      fail("a pre-feature chain-position entry (no user_preset key) must read back null, not throw or invent one");
    }
    /* Absent entry entirely (e.g. chain.synth is null on an empty slot). */
    if (entryUserPreset(null) !== null) fail("a null chain-position entry must read back null");
    if (entryUserPreset(undefined) !== null) fail("an undefined chain-position entry must read back null");

    /* A real record round-trips. */
    const got = entryUserPreset({ user_preset: { name: "Fat Brass", hash: "abc:3" } });
    if (!got || got.name !== "Fat Brass" || got.hash !== "abc:3") {
      fail("a real user_preset entry did not round-trip through entryUserPreset");
    }

    /* A record with a name but no hash (older/partial write) still yields a
       usable record rather than being dropped, per makeRecord/isModified in
       current_preset.mjs treating a null hash as "cannot tell, never a star". */
    const noHash = entryUserPreset({ user_preset: { name: "Fat Brass" } });
    if (!noHash || noHash.name !== "Fat Brass" || noHash.hash !== null) {
      fail("a user_preset entry with no hash should still read back a record with hash: null");
    }

    /* An entry whose user_preset has no name at all is not a real record. */
    if (entryUserPreset({ user_preset: { hash: "abc:3" } }) !== null) {
      fail("a user_preset object with no name must not read back as a record");
    }
  }
}

/* ---- structural: the boot-restore loop actually calls the reader, once
   per chain position, and CLEARS when absent (setUserPresetRecord is called
   unconditionally with entryUserPreset(...), not gated by an `if (record)`
   that would leave a stale entry from a previous load into this slot) ----- */
{
  const marker = "Sync slot names + per-component bypass from autosave if present";
  const at = src.indexOf(marker);
  if (at < 0) { fail("boot-restore block (marker comment) not found"); }
  else {
    const end = src.indexOf("saveSlotsToConfig(slots);", at);
    const block = src.slice(at, end);
    if (!/setUserPresetRecord\(i,\s*"synth",\s*entryUserPreset\(/.test(block)) {
      fail("boot restore does not call setUserPresetRecord for the synth position");
    }
    if (!/setUserPresetRecord\(i,\s*`midi_fx\$\{mf \+ 1\}`,\s*entryUserPreset\(/.test(block)) {
      fail("boot restore does not call setUserPresetRecord for midi_fx positions");
    }
    if (!/setUserPresetRecord\(i,\s*`fx\$\{fx \+ 1\}`,\s*entryUserPreset\(/.test(block)) {
      fail("boot restore does not call setUserPresetRecord for audio_fx positions");
    }
    /* Every call passes entryUserPreset(...) directly as the record argument
       (not `x || undefined` or similar) -- so a null result CLEARS via
       setUserPresetRecords own `if (record) ... else delete` branch, rather
       than a stale record surviving because nothing overwrote it. */
    if (/if\s*\(chain[^)]*\.synth\.user_preset\)[\s\S]{0,80}setUserPresetRecord/.test(block)) {
      fail("boot restore looks conditionally gated before calling setUserPresetRecord -- " +
           "that would leave a stale record instead of clearing it");
    }
  }
}

/* ---- behavioural: setUserPresetRecord/getUserPresetRecord clear correctly */
{
  const at = src.indexOf("const currentUserPresets = Object.create(null)");
  const end = src.indexOf("\n}\n", src.indexOf("function entryUserPreset("));
  if (at < 0 || end < 0) {
    fail("could not lift the record store block");
  } else {
    const store = new Function(
      src.slice(at, end + 2) +
      "\nreturn { getUserPresetRecord, setUserPresetRecord, entryUserPreset };")();
    if (store.getUserPresetRecord(0, "synth") !== null) fail("a fresh store must read back null");
    store.setUserPresetRecord(0, "synth", { name: "A", hash: "h:1" });
    if (store.getUserPresetRecord(0, "synth").name !== "A") fail("set then get did not round-trip");
    if (store.getUserPresetRecord(1, "synth") !== null) fail("record leaked across slots -- keyed wrong");
    if (store.getUserPresetRecord(0, "fx1") !== null) fail("record leaked across prefixes -- keyed wrong");
    store.setUserPresetRecord(0, "synth", null);
    if (store.getUserPresetRecord(0, "synth") !== null) fail("setUserPresetRecord(..., null) must clear, not leave the old record");
  }
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS");
'
