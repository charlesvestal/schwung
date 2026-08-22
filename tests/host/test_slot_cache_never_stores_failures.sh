#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The slot param cache must never store a FAILED read.
#
# null from getSlotParam is the third answer -- the read did not complete --
# and it is not news about the module. Cached, it turns a momentary channel
# stall into a 500ms lie about the chain.
#
# Reported from the device: "i just had a blank slot after loading granny. had
# to switch slots a few times to get it to come back." granny reads its WAV
# synchronously inside set_param, on the SPI thread that also serves param
# requests, so every read during the load fails; the blank was then held by the
# cache, keyed by slot, so returning to the slot hit the same poisoned entry.
#
# "" is a different answer and IS cached: the channel served us, the key
# produced nothing.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
const src = require("fs").readFileSync("src/shadow/shadow_ui.js", "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

const at = src.indexOf("function getSlotParamCached(");
if (at < 0) fail("getSlotParamCached is gone");
const body = src.slice(at, src.indexOf("\n}\n", at) + 2);

/* Lifted with its two dependencies so the real logic runs. */
let store = {};
let answer = null;
/* TTL 0 so every call actually READS. With the real 500ms the entry written by
 * the good read is still warm, the stall call returns from cache without ever
 * calling getSlotParam, and the failure path is never exercised -- which let a
 * "cache failures again" mutation survive the first version of this test. */
const make = (ttl) => new Function(
  "slotParamCache", "SLOT_PARAM_CACHE_TTL_MS", "getSlotParam",
  body + "\nreturn getSlotParamCached;"
)(store, ttl === undefined ? 0 : ttl, () => answer);

/* A good read is cached. */
store = {}; answer = "granny";
let f = make();
if (f(0, "synth_module", "granny") !== "granny") fail("a good read was not returned");
if (!store["0:synth_module"]) fail("a good read was not cached");

/* A FAILED read must not overwrite it, and must not be stored. */
answer = null;
const during = f(0, "synth_module", "granny");
if (store["0:synth_module"].value === null)
  fail("a failed read was written into the cache -- the slot now reads blank for the whole TTL");
if (during === null)
  fail("a failed read returned null even though a known-good value for the same module was cached");
if (during !== "granny")
  fail("expected the last known-good value during a stall, got " + JSON.stringify(during));

/* "" is served-but-empty and IS a value. */
store = {}; answer = "";
f = make();
if (f(1, "fx1_module", "x") !== "") fail("an empty answer was not returned");
if (!store["1:fx1_module"]) fail("an empty answer was not cached — it is a real answer");

/* With nothing cached, a failure is reported as a failure rather than
   invented as empty. */
store = {}; answer = null;
f = make();
const cold = f(2, "synth_module", "granny");
if (cold === "") fail("a failed read with a cold cache was turned into \"\" — that is the " +
                      "collapse of null and empty the tri-state rule exists to stop");

console.log("  ok  a failed read is neither cached nor allowed to overwrite a good one");
console.log("  ok  during a stall the last known-good value for the same module is used");
console.log("  ok  \"\" is still treated as a real answer and cached");
console.log("PASS: the slot cache never stores a read that did not complete");
'
