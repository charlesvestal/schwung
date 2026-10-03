#!/usr/bin/env bash
# A SET SWITCH MUST NOT SAVE THE OLD SET'S MODULE INTO THE NEW SET.
#
# Slot loads run on the shim's loader thread since #605, so a set-switch
# restore write holds the param channel for a fade plus the module's own
# create/destroy. The restore gave each write 1.5 s, ignored the clear's
# result, and on a load_file timeout SENT IT AGAIN -- a second full load of a
# module still being built. On a Move a switch left JE-8086 in a slot whose
# file said Mini-JV, and the next autosave wrote JE-8086 into that file.
#
# Now: budgets sized for the loader, every slot confirmed by READING IT BACK
# against its file, a retry only on a confirmed mismatch, and autosave holding
# any slot that still carries the outgoing set's modules. This lifts the two
# pure helpers and runs them against a fake param channel, and pins the rest.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
node - "$ROOT/src/shadow/shadow_ui.js" <<'JS'
const fs = require("fs");
const src = fs.readFileSync(process.argv[2], "utf8");
let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };
const lift = (name) => {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " not found"); return ""; }
  let depth = 0, i = src.indexOf("{", at);
  for (; i < src.length; i++) {
    if (src[i] === "{") depth++;
    else if (src[i] === "}" && --depth === 0) break;
  }
  return src.slice(at, i + 1);
};

/* ---- slotFileExpectation ---- */
const env = new Function("MAX_MIDI_FX", "MAX_FX", "getSlotParam",
  lift("slotFileExpectation") + "\n" + lift("slotMatchesExpectation") +
  "\nreturn { slotFileExpectation, slotMatchesExpectation };");
let params = {};
const api = env(4, 8, (slot, key) => (key in params ? params[key] : null));

/* The shape autosave WRITES: the synth names its module `module`, an FX entry
   names its own `type`. A fixture written from assumption instead let the
   first version read `module` for FX and pass here while every slot with an
   audio FX failed to confirm on the device. */
const file = JSON.stringify({ chain: { synth: { module: "minijv", config: {} },
  audio_fx: [null, { type: "freeverb", params: {}, bypassed: 0 }],
  midi_fx: [{ type: "arp", params: {} }] } });
const exp = api.slotFileExpectation(file);
if (!exp || exp.synth !== "minijv" || exp.fx.join(",") !== ",freeverb" || exp.midiFx.join(",") !== "arp")
  fail("expectation of a chain file: " + JSON.stringify(exp));
const empty = api.slotFileExpectation("{}\n");
if (!empty || empty.synth !== "" || empty.fx.length) fail("a near-empty file is an EMPTY slot");
if (api.slotFileExpectation(null).synth !== "") fail("a missing file is an EMPTY slot");
if (api.slotFileExpectation("{ not json, but long enough") !== null)
  fail("an unparseable file must be null -- nothing can be confirmed against it");

if (!/patch\.audio_fx\.push\(\{\s*type: moduleData\.module/.test(src) ||
    !/patch\.midi_fx\.push\(\{\s*type: moduleData\.module/.test(src))
  fail("the autosave writer no longer names an FX by `type` -- update slotFileExpectation with it");

/* ---- slotMatchesExpectation ---- */
const live = (o) => { params = Object.assign({ midi_fx_count: "1", midi_fx1_module: "arp", fx_count: "0" }, o); };
live({ synth_module: "minijv", midi_fx_count: "1", midi_fx1_module: "arp", fx_count: "2", fx1_module: "", fx2_module: "freeverb" });
if (api.slotMatchesExpectation(0, exp) !== true) fail("the restored slot must match");
live({ synth_module: "jp8000", fx_count: "2", fx1_module: "", fx2_module: "freeverb" });
if (api.slotMatchesExpectation(0, exp) !== false) fail("the OUTGOING synth must not match (the bug)");
live({ synth_module: "minijv", fx_count: "2", fx1_module: "cloudseed", fx2_module: "freeverb" });
if (api.slotMatchesExpectation(0, exp) !== false) fail("an outgoing FX left in a position the file has empty");
live({ synth_module: "minijv", fx_count: "3", fx1_module: "", fx2_module: "freeverb", fx3_module: "gate" });
if (api.slotMatchesExpectation(0, exp) !== false) fail("a live FX past the file's list must be empty");
live({ fx_count: "2" });
if (api.slotMatchesExpectation(0, exp) !== null) fail("a failed read is null, never a verdict");

/* ---- the restore and the autosave guard ---- */
const pass2 = src.slice(src.indexOf("Pass 2: Load new state for non-empty slots"),
                        src.indexOf("Pass 3: the incoming set's automation lanes"));
if (/"load_file", path, 1500/.test(src) || /"load_file", path, 3000/.test(src))
  fail("the 1.5 s / 3 s load_file budget is back -- loads run on the loader now");
if (!/slotMatchesExpectation\(i, exp\)/.test(pass2)) fail("pass 2 no longer reads each slot back");
const retryAt = pass2.indexOf("(retry)");
if (retryAt < 0 || pass2.lastIndexOf("confirmed === false", retryAt) < 0)
  fail("pass 2 may only retry on a CONFIRMED mismatch, never on a bare timeout");
const auto = lift("autosaveOneSlot");
const holdAt = auto.indexOf("if (slotRestoreHolds(i)) return;");
const sigAt = auto.indexOf("const currentSig = getSlotModuleSignature(i)");
if (holdAt < 0) fail("autosaveOneSlot does not return on slotRestoreHolds(i)");
else if (sigAt >= 0 && holdAt > sigAt) fail("the restore guard must run before anything is saved");

/* slotRestoreHolds, run: the cases that decide whether a file is overwritten */
const holds = new Function("slotRestorePending", "slotMatchesExpectation", "getSlotModuleSignature",
  lift("slotRestoreHolds") + "\nreturn slotRestoreHolds;");
const run = (pending, match, sig) => {
  const tbl = [pending];
  const h = holds(tbl, () => match, () => sig);
  return { held: h(0), left: tbl[0] };
};
const P = () => ({ exp: {}, outgoing: "jp8000|/" });
let r = run(null, false, "x");
if (r.held) fail("no pending restore must never hold");
r = run(P(), false, "jp8000|/");
if (!r.held || !r.left) fail("still the OUTGOING modules: must hold and stay pending (the bug)");
r = run(P(), null, "minijv|/");
if (!r.held || !r.left) fail("an unknown match must hold");
r = run(P(), false, null);
if (!r.held || !r.left) fail("an unknown signature must hold");
r = run(P(), true, "minijv|/");
if (r.held || r.left) fail("a match confirms: save, and drop the entry");
r = run(P(), false, "dexed|/");
if (r.held || r.left) fail("something ELSE there (the user changed it): save, and drop the entry");

if (failures) { console.log(failures + " failure(s)"); process.exit(1); }
console.log("PASS: set restore is confirmed, and autosave holds an unconfirmed slot");
JS
