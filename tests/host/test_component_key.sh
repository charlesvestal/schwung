#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
node --input-type=module -e '
import { isComponentParamKey, isComponentTarget } from "./src/shared/component_key.mjs";
import { readFileSync } from "node:fs";

let fails = 0;
const check = (c, msg) => { if (!c) { console.log("FAIL: " + msg); fails++; } };

/* THE KEY THAT CAUSED THIS. A lock-map QUESTION was converted into a p-lock,
 * which marked the step press spent, which stopped Move ever seeing the tap. */
check(!isComponentParamKey("lanes:step_locks_query"),
      "lanes:step_locks_query is a question and must never convert to a p-lock");
check(!isComponentParamKey("lanes:probe"), "lanes:probe must not convert");
check(!isComponentParamKey("lanes:plock_step"),
      "the conversion target itself must not be convertible -- that is a loop");
check(!isComponentParamKey("buses:main_send1"), "buses: is not a component");
check(!isComponentParamKey("master_fx:x"), "master_fx: is not a chain component here");

check(isComponentParamKey("synth:cutoff"), "synth:cutoff is a component param");
check(isComponentParamKey("fx3:mix"), "fx3:mix is a component param");
check(isComponentParamKey("midi_fx1:rate"), "midi_fx1:rate is a component param");

check(!isComponentParamKey("synth"), "a key with no colon names no param");
check(!isComponentParamKey("synth:"), "a key with an empty param half names no param");
check(!isComponentTarget("fx"), "a bare fx carries no position");
check(!isComponentTarget("fxA"), "fxA is not a position");
check(!isComponentTarget("midi_fx"), "a bare midi_fx carries no position");
check(isComponentTarget("fx12"), "fx12 is a position");

/* AND IT IS THE SHIM'"'"'S RULE, not a second one. shadow_component_param_split is
 * the authority; this asserts the JS agrees with what that function accepts,
 * so the two cannot drift into disagreeing about what a p-lock may convert. */
const c = readFileSync("src/host/shadow_chain_mgmt.c", "utf8");
const fn = c.match(/static int shadow_component_param_split\(const char \*key\)\n\{[\s\S]*?\n\}/);
check(!!fn, "could not find shadow_component_param_split in the shim");
if (fn) {
  const body = fn[0];
  check(/strncmp\(key, "synth", 5\)/.test(body), "the shim no longer accepts synth -- the JS rule is stale");
  check(/strncmp\(key, "fx", 2\)/.test(body), "the shim no longer accepts fx<N> -- the JS rule is stale");
  check(/strncmp\(key, "midi_fx", 7\)/.test(body), "the shim no longer accepts midi_fx<N> -- the JS rule is stale");
  /* No FOURTH prefix has appeared without the JS learning about it. */
  const prefixes = [...body.matchAll(/strncmp\(key, "([a-z_]+)"/g)].map(m => m[1]).sort();
  const want = ["fx", "midi_fx", "synth"];
  check(JSON.stringify(prefixes) === JSON.stringify(want),
        "the shim accepts " + JSON.stringify(prefixes) + " but component_key.mjs knows " +
        JSON.stringify(want) + " -- a prefix was added on one side only");
}

if (fails) { console.log(fails + " failure(s)"); process.exit(1); }
console.log("PASS: component_key — a question never becomes an edit");
'
