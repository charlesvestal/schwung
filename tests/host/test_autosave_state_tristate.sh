#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A slot's autosave must survive a module that declares no `state`.
#
# getSlotStateWithRetry tested the answer with `if (state)`, which collapses
# "" (the channel served us; the module declares no `state` key) into null
# (the read did not complete). buildSlotPatchJson then bailed to protect a
# good file from a timed-out read — and abandoned the WHOLE slot, including
# the other components in it.
#
# denis and branchage implement no `state`. A slot containing either never
# autosaved anything, ever, and neither did the FX behind it. Found in the
# 2026-08 fleet audit; it is the same tri-state rule as the contract reads in
# page_controller.mjs, one layer up.
#
# The retry count matters as much as the return value: retrying a served ""
# spends three IPC round trips (~2.8 ms each) on every autosave pass to learn
# something the first read already said.

file="src/shadow/shadow_ui.js"

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

# ---- 1. behaviour: lift the function and drive it against a fake channel ----
fn=$(awk '/^function getSlotStateWithRetry\(/,/^}/' "$file")
if [ -z "$fn" ]; then
  echo "FAIL: getSlotStateWithRetry not found in $file" >&2
  exit 1
fi

node -e '
const fs = require("fs");
const src = fs.readFileSync("/dev/stdin", "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

/* Lift it with an explicit dependency list, the same idiom the knob-card read
 * budget test uses: a free identifier here would silently become undefined and
 * the assertions would measure nothing. */
let reads = 0;
let answer = null;
const getSlotParam = () => { reads++; return answer; };
const debugLog = () => {};
const make = new Function("getSlotParam", "debugLog",
                          src + "; return getSlotStateWithRetry;");
const getSlotStateWithRetry = make(getSlotParam, debugLog);

/* A module that declares no state: served, empty, ONE read, no retries. */
reads = 0; answer = "";
let got = getSlotStateWithRetry(0, "synth:state", 3);
if (got !== "") fail("a served empty state came back as " + JSON.stringify(got) + ", expected \"\"");
if (got === null) fail("a served empty state was reported as a failed read");
if (reads !== 1) fail("a served empty state cost " + reads + " reads, expected 1 (retrying it is pure IPC waste)");

/* A failed read: still retried, still null at the end. */
reads = 0; answer = null;
got = getSlotStateWithRetry(0, "synth:state", 3);
if (got !== null) fail("a failed read came back as " + JSON.stringify(got) + ", expected null");
if (reads !== 4) fail("a failed read cost " + reads + " reads, expected 1 + 3 retries");

/* A real answer passes straight through. */
reads = 0; answer = "{\"a\":1}";
got = getSlotStateWithRetry(0, "synth:state", 3);
if (got !== "{\"a\":1}") fail("a real state was not returned verbatim: " + JSON.stringify(got));
if (reads !== 1) fail("a real state cost " + reads + " reads, expected 1");

/* A read that fails once then is served empty must stop there, not spin. */
reads = 0;
const seq = [null, ""];
const getSlotParam2 = () => { reads++; return seq[Math.min(reads - 1, seq.length - 1)]; };
const g2 = new Function("getSlotParam", "debugLog",
                        src + "; return getSlotStateWithRetry;")(getSlotParam2, debugLog);
got = g2(0, "synth:state", 3);
if (got !== "") fail("null-then-empty came back as " + JSON.stringify(got) + ", expected \"\"");
if (reads !== 2) fail("null-then-empty cost " + reads + " reads, expected 2");

console.log("  ok  getSlotStateWithRetry keeps \"\" and null apart");
' <<< "$fn"

# ---- 2. the caller must branch on the RAW value, not on truthiness ---------
# componentEntry is a closure inside buildSlotPatchJson, so this half is a
# source pin. `else if (bailIfEmpty)` alone is the bug: it fires for "" too.
entry=$(awk '/const componentEntry = /,/^    };/' "$file")
if [ -z "$entry" ]; then
  echo "FAIL: componentEntry not found in $file" >&2
  exit 1
fi
if ! grep -Eq 'stateJson === null[[:space:]]*&&[[:space:]]*bailIfEmpty' <<<"$entry"; then
  echo "FAIL: componentEntry does not require a NULL state before bailing." >&2
  echo "      Bailing on \"\" abandons the whole slot for any module that" >&2
  echo "      declares no state key (denis, branchage)." >&2
  exit 1
fi
echo "  ok  componentEntry bails only on a FAILED read"

# ---- 3. whole-slot behaviour: a stateful FX survives a stateless neighbour --
# PALETTE serialises its four effect selections, amounts, macros, drift values,
# global mix/input/reorder/preset, feedback and tempo controls as one opaque CSV
# state value.  Schwung 0.12.1 could abandon that value when an earlier module
# in the same slot served "" for state.  Exercise the actual patch builder so
# this regression covers the user-visible chain, not only the retry helper.
builder=$(awk '/^function buildSlotPatchJson\(/,/^}/' "$file")
if [ -z "$builder" ]; then
  echo "FAIL: buildSlotPatchJson not found in $file" >&2
  exit 1
fi

node -e '
const fs = require("fs");
const src = fs.readFileSync("/dev/stdin", "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
const paletteState =
  "2,0.2500,0.5000,0.1000,5,0.7500,0.2000,0.0000," +
  "0,0.0000,0.0000,0.0000,11,1.0000,0.9000,0.3000," +
  "0.4200,1.2500,7,3,0.1500,1,96,4";
const chainConfigs = [{
  synth: { module: "stateless-synth", params: { voice: "kept" } },
  midiFx: [],
  fx: [{ module: "palette", params: {} }]
}];
const answers = {
  "synth:state": "",
  "synth:bypassed": "0",
  "fx1:state": paletteState,
  "fx1:bypassed": "0",
  "slot:receive_channel": "2",
  "slot:forward_channel": "3",
  "midi_fx_pre_mode": "1",
  "knob_mappings": "[]",
  "lfo_config": ""
};
let reads = [];
const getSlotParam = (_slot, key) => {
  reads.push(key);
  return Object.prototype.hasOwnProperty.call(answers, key) ? answers[key] : "";
};
const debugLog = () => {};
const make = new Function("chainConfigs", "getSlotParam", "debugLog",
  src + "; return buildSlotPatchJson;");
const buildSlotPatchJson = make(chainConfigs, getSlotParam, debugLog);
const patchJson = buildSlotPatchJson(0, "Persistence", true, false);
if (!patchJson) fail("a stateless neighbour still abandoned the whole slot");
const patch = JSON.parse(patchJson);
if (!patch.synth || patch.synth.config.voice !== "kept")
  fail("the stateless component fallback params were lost");
if (!patch.audio_fx || patch.audio_fx.length !== 1)
  fail("PALETTE disappeared from the saved chain");
if (patch.audio_fx[0].params.state !== paletteState)
  fail("PALETTE opaque state changed in autosave");
if (reads.filter((k) => k === "synth:state").length !== 1)
  fail("the stateless component was retried despite serving an answer");

/* A failed PALETTE read is different: preserve the prior on-disk slot rather
 * than writing defaults. This is why the empty/null distinction is necessary. */
answers["fx1:state"] = null;
reads = [];
if (buildSlotPatchJson(0, "Persistence", true, false) !== null)
  fail("a failed PALETTE state read did not protect the existing autosave");
if (reads.filter((k) => k === "fx1:state").length !== 2)
  fail("autosave did not make exactly one bounded retry after a failed state read");

console.log("  ok  PALETTE state survives a stateless component in the same slot");
' <<< "$fn
$builder"

echo "PASS: complete component state survives mixed-chain autosave"
