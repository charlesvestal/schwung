#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The loaded-preset record rides the slot autosave, not a param or SHM field --
# it is pure UI bookkeeping the DSP never sees (see current_preset.mjs). This
# pins three things: buildSlotPatchJson writes {name, hash} from INSIDE
# componentEntry, conditionally, threaded through all three chain-position
# shapes (synth/config, midi_fx/params, audio_fx/params); the shared reader
# syncUserPresetRecordsFromChain reads it back per component prefix, clearing
# any stale record when a patch carries none; and BOTH slot-load paths call
# that ONE definition -- the boot-restore loop in init(), and the
# SHADOW_UI_FLAG_SET_CHANGED "Pass 2" handler in tick(), which reloads a slot
# in-session on an Ableton set switch with no reboot. The map is keyed
# (slot, prefix), not by set, so a path that skips the sync leaves the
# OUTGOING set's record attached to the INCOMING set's same-numbered slot --
# this repo has already shipped one bug of exactly that shape (a stale global
# slot_state propagating into every set).
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
/* ccOverridesSeen is the per-boot memory that stops an empty live CC map
   overwriting a saved one -- see docs/CC_MAP.md. It is a free identifier in
   buildSlotPatchJson, so the lift has to be handed it like the rest. */
const deps = ["chainConfigs", "getSlotStateWithRetry", "debugLog", "getSlotParam", "getUserPresetRecord", "ccOverridesSeen"];
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
  const ccOverridesSeen = [];   /* nothing seen on disk in this harness */
  const debugLog = () => {};

  /* Case A: nothing loaded a preset anywhere in this slot -- absent means
     absent, on every one of the three shapes. */
  const noRecord = () => null;
  const buildNone = buildSlotPatchJsonMaker(chainConfigs, getSlotStateWithRetry, debugLog, getSlotParam, noRecord, ccOverridesSeen);
  const patchNone = JSON.parse(buildNone(0, "Name", false, false));
  if ("user_preset" in patchNone.synth) fail("no preset loaded, but synth carries a user_preset key");
  if ("user_preset" in patchNone.midi_fx[0]) fail("no preset loaded, but midi_fx carries a user_preset key");
  if ("user_preset" in patchNone.audio_fx[0]) fail("no preset loaded, but audio_fx carries a user_preset key");

  /* Case B: a preset is loaded on the synth only -- the OTHER two positions
     must not pick it up (keyed by slot+prefix, not by slot alone). */
  const onlySynth = (slot, prefix) => (prefix === "synth" ? { name: "Fat Brass", hash: "abc:3" } : null);
  const buildSynth = buildSlotPatchJsonMaker(chainConfigs, getSlotStateWithRetry, debugLog, getSlotParam, onlySynth, ccOverridesSeen);
  const patchSynth = JSON.parse(buildSynth(0, "Name", false, false));
  if (!patchSynth.synth.user_preset || patchSynth.synth.user_preset.name !== "Fat Brass") {
    fail("synth preset record did not reach the saved patch");
  }
  if (patchSynth.synth.user_preset.hash !== "abc:3") fail("synth preset hash did not reach the saved patch");
  if ("user_preset" in patchSynth.midi_fx[0]) fail("midi_fx picked up the synth preset record -- keys are not per-prefix");
  if ("user_preset" in patchSynth.audio_fx[0]) fail("audio_fx picked up the synth preset record -- keys are not per-prefix");

  /* Case C: a preset on the audio FX only. */
  const onlyFx = (slot, prefix) => (prefix === "fx1" ? { name: "Warm Plate", hash: "def:5" } : null);
  const buildFx = buildSlotPatchJsonMaker(chainConfigs, getSlotStateWithRetry, debugLog, getSlotParam, onlyFx, ccOverridesSeen);
  const patchFx = JSON.parse(buildFx(0, "Name", false, false));
  if (!patchFx.audio_fx[0].user_preset || patchFx.audio_fx[0].user_preset.name !== "Warm Plate") {
    fail("audio_fx preset record did not reach the saved patch");
  }
  if ("user_preset" in patchFx.synth) fail("synth picked up the fx preset record");

  /* Case D: the BAIL guard still fires and abandons the WHOLE save -- a
     record must not paper over a failed state read. */
  const failingState = (slot, key) => (key === "synth:state" ? null : "");
  const buildBailed = buildSlotPatchJsonMaker(chainConfigs, failingState, debugLog, getSlotParam,
    () => ({ name: "Should Not Matter", hash: "x:1" }), ccOverridesSeen);
  const bailed = buildBailed(0, "Name", true, false);
  if (bailed !== null) fail("a failed state read with bailIfEmpty must still abandon the whole save (got non-null)");

  /* Case E: MULTI-POSITION -- two midi_fx entries, a record on the first and
     none on the second. Every fixture above is single-element per array, so
     an index bug (e.g. always reading position 0, or off-by-one against the
     1-based `${id}` prefix) would pass them all and still be wrong here. */
  const cfg2 = {
    synth: { module: "granny", params: {} },
    midiFx: [{ module: "chord", params: {} }, { module: "arp", params: {} }],
    fx: []
  };
  const chainConfigs2 = { 0: cfg2 };
  const state2 = { "synth:state": "S", "midi_fx1:state": "M1", "midi_fx2:state": "M2" };
  const getSlotStateWithRetry2 = (slot, key) => (key in state2 ? state2[key] : "");
  const firstMidiOnly = (slot, prefix) => (prefix === "midi_fx1" ? { name: "Swing 8th", hash: "m1:2" } : null);
  const buildMulti = buildSlotPatchJsonMaker(chainConfigs2, getSlotStateWithRetry2, debugLog, getSlotParam, firstMidiOnly, ccOverridesSeen);
  const patchMulti = JSON.parse(buildMulti(0, "Name", false, false));
  if (!patchMulti.midi_fx[0].user_preset || patchMulti.midi_fx[0].user_preset.name !== "Swing 8th") {
    fail("multi-position: midi_fx[0] record did not reach the saved patch");
  }
  if ("user_preset" in patchMulti.midi_fx[1]) {
    fail("multi-position: midi_fx[1] (no record) picked up a user_preset key -- index bug");
  }
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

/* ---- structural: exactly ONE definition of the sync helper -- a second
   copy of "walk synth + every midi_fx<n> + every fx<n>" is exactly the kind
   of defect this codebase treats as a bug in itself. ---------------------- */
{
  const count = (src.match(/function syncUserPresetRecordsFromChain\(/g) || []).length;
  if (count !== 1) {
    fail("expected exactly 1 definition of syncUserPresetRecordsFromChain, found " + count);
  }
}

/* ---- structural: the helper is bounded by the published caps, not by the
   saved array length -- a `.length &&` guard would leave stale entries PAST
   whatever the old set happened to save, instead of clearing every position
   up to the cap. ------------------------------------------------------------ */
{
  const at = src.indexOf("function syncUserPresetRecordsFromChain(");
  const end = src.indexOf("\n}\n", at);
  if (at < 0 || end < 0) {
    fail("syncUserPresetRecordsFromChain is gone -- the record would be write-only");
  } else {
    const body = src.slice(at, end);
    if (!/for \(let mf = 0; mf < MAX_MIDI_FX; mf\+\+\)/.test(body)) {
      fail("syncUserPresetRecordsFromChain must walk midi_fx bounded by MAX_MIDI_FX alone, not array length");
    }
    if (!/for \(let fx = 0; fx < MAX_FX; fx\+\+\)/.test(body)) {
      fail("syncUserPresetRecordsFromChain must walk audio_fx bounded by MAX_FX alone, not array length");
    }
    if (!/setUserPresetRecord\(slotIndex,\s*"synth",\s*entryUserPreset\(/.test(body)) {
      fail("syncUserPresetRecordsFromChain does not sync the synth position");
    }
  }
}

/* ---- structural: BOTH slot-load paths call the ONE helper -- boot-restore
   in init(), and the SHADOW_UI_FLAG_SET_CHANGED "Pass 2" handler in tick(),
   which reloads a slot in-session on a set switch with no reboot. Skipping
   either leaves a record from the OLD set attached to the NEW sets
   same-numbered slot. ------------------------------------------------------ */
{
  const bootMarker = "Sync slot names + per-component bypass from autosave if present";
  const bootAt = src.indexOf(bootMarker);
  if (bootAt < 0) { fail("boot-restore block (marker comment) not found"); }
  else {
    const bootEnd = src.indexOf("saveSlotsToConfig(slots);", bootAt);
    const bootBlock = src.slice(bootAt, bootEnd);
    if (!/syncUserPresetRecordsFromChain\(i, chain\)/.test(bootBlock)) {
      fail("boot restore no longer calls syncUserPresetRecordsFromChain -- write-only regression");
    }
  }

  const setChangedAt = src.indexOf("Pass 2: Load new state for non-empty slots");
  if (setChangedAt < 0) { fail("SET_CHANGED Pass 2 block (marker comment) not found"); }
  else {
    const setChangedEnd = src.indexOf("Refresh UI state immediately", setChangedAt);
    const setChangedBlock = src.slice(setChangedAt, setChangedEnd);
    const calls = (setChangedBlock.match(/syncUserPresetRecordsFromChain\(/g) || []).length;
    /* One call per branch: loaded+parsed, loaded+parse-failed, empty state,
       no state file -- so a set switch that lands a slot in ANY of those
       branches still syncs (parse-failure and "no file" must CLEAR, which is
       exactly the shape of the bug this closes: an old record surviving
       because the new slot has nothing to overwrite it with). */
    if (calls < 4) {
      fail("SET_CHANGED Pass 2 calls syncUserPresetRecordsFromChain " + calls +
           " time(s), expected at least 4 -- some branch (parsed / parse-failed / " +
           "empty / no-file) is not syncing, so a record can survive a set switch");
    }
    if (!/syncUserPresetRecordsFromChain\(i, chain\)/.test(setChangedBlock)) {
      fail("SET_CHANGED Pass 2 never syncs from a successfully parsed chain");
    }
    if (!/syncUserPresetRecordsFromChain\(i, null\)/.test(setChangedBlock)) {
      fail("SET_CHANGED Pass 2 never clears (sync with null) on an empty/missing/unparseable slot");
    }
  }
}

/* ---- behavioural: run the REAL syncUserPresetRecordsFromChain + its store,
   lifted together (it closes over getUserPresetRecord/setUserPresetRecord/
   entryUserPreset, which are defined right above it in the same block, and
   over the imported MAX_MIDI_FX/MAX_FX, passed in as params). ------------- */
{
  const storeAt = src.indexOf("const currentUserPresets = Object.create(null)");
  const syncAt = src.indexOf("function syncUserPresetRecordsFromChain(");
  const syncEnd = src.indexOf("\n}\n", syncAt);
  if (storeAt < 0 || syncAt < 0 || syncEnd < 0) {
    fail("could not lift the record store + sync-helper block");
  } else {
    const world = new Function("MAX_MIDI_FX", "MAX_FX",
      src.slice(storeAt, syncEnd + 2) +
      "\nreturn { getUserPresetRecord, setUserPresetRecord, entryUserPreset, syncUserPresetRecordsFromChain };"
    )(8, 8);

    /* Store isolation (unchanged from before the extraction). */
    if (world.getUserPresetRecord(0, "synth") !== null) fail("a fresh store must read back null");
    world.setUserPresetRecord(0, "synth", { name: "A", hash: "h:1" });
    if (world.getUserPresetRecord(0, "synth").name !== "A") fail("set then get did not round-trip");
    if (world.getUserPresetRecord(1, "synth") !== null) fail("record leaked across slots -- keyed wrong");
    if (world.getUserPresetRecord(0, "fx1") !== null) fail("record leaked across prefixes -- keyed wrong");
    world.setUserPresetRecord(0, "synth", null);
    if (world.getUserPresetRecord(0, "synth") !== null) fail("setUserPresetRecord(..., null) must clear, not leave the old record");

    /* MULTI-POSITION read side: midi_fx[0] has a record, midi_fx[1] does not
       -- same index-bug guard as the write-side Case E above, but for the
       reader this time. */
    world.syncUserPresetRecordsFromChain(2, {
      synth: { user_preset: { name: "Fat Brass", hash: "s:1" } },
      midi_fx: [{ user_preset: { name: "Swing 8th", hash: "m1:2" } }, { module: "arp" }],
      audio_fx: []
    });
    const mf1 = world.getUserPresetRecord(2, "midi_fx1");
    const mf2 = world.getUserPresetRecord(2, "midi_fx2");
    if (!mf1 || mf1.name !== "Swing 8th") fail("multi-position read: midi_fx1 record did not restore");
    if (mf2 !== null) fail("multi-position read: midi_fx2 (no record in the saved chain) restored a record anyway -- index bug");

    /* CLEAR ON RESYNC -- the whole point of the fix. Seed a record on fx3
       (simulating the OUTGOING sets slot 2), then sync as if the INCOMING
       sets slot 2 has nothing there. It must not survive. */
    world.setUserPresetRecord(2, "fx3", { name: "Stale From Old Set", hash: "x:9" });
    if (world.getUserPresetRecord(2, "fx3") === null) fail("test setup broken -- seed did not take");
    world.syncUserPresetRecordsFromChain(2, { synth: null, midi_fx: [], audio_fx: [] });
    if (world.getUserPresetRecord(2, "fx3") !== null) {
      fail("a stale record from a previous load into this slot survived a resync with an empty chain -- " +
           "this is the exact cross-set leak the SET_CHANGED fix closes");
    }
    if (world.getUserPresetRecord(2, "synth") !== null) fail("resync with chain.synth=null must clear the synth record too");
    if (world.getUserPresetRecord(2, "midi_fx1") !== null) fail("resync with an empty midi_fx array must clear midi_fx1 too");

    /* A totally absent chain (e.g. an unparseable slot_N.json) clears every
       position, same as an explicitly empty one. */
    world.setUserPresetRecord(3, "synth", { name: "Also Stale", hash: "y:1" });
    world.syncUserPresetRecordsFromChain(3, null);
    if (world.getUserPresetRecord(3, "synth") !== null) fail("syncUserPresetRecordsFromChain(slot, null) must clear the synth position");
  }
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS");
'
