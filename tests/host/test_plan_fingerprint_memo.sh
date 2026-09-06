#!/usr/bin/env bash
# planPages must not re-hash a contract it has already hashed.
#
# The fingerprint is a JSON.stringify of the whole contract plus a walk over
# every character of it, and planPages runs on EVERY DETENT of a gating knob
# (replanIfCondition). On those re-plans only a value changed; the contract
# objects are the same ones, because the controller assigns them from parse()
# and never mutates them in place. At 94 KB that hash was ~0.78 ms in node and
# far worse under QuickJS on the device, and the screen stalled while a filter
# type knob turned.
#
# The memo is keyed on IDENTITY, so this test asserts both halves: the same
# objects skip the hash, and objects that are genuinely new do not.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { planPages } from "./src/shared/param_pages/page_plan.mjs";

const hierarchy = { levels: { root: { knobs: ["a"], params: [{ key: "a" }] } } };
const chainParams = [{ key: "a", name: "A", type: "int", min: 0, max: 9 }];
const fp = () => planPages({ hierarchy, chainParams }).fingerprint;

const first = fp();
if (!first) { console.log("FAIL: no fingerprint returned"); process.exit(1); }
if (fp() !== first) { console.log("FAIL: same objects gave two fingerprints"); process.exit(1); }

// Mutating IN PLACE must not be seen -- which is the memo doing its job, and
// is safe only because the controller never does this. If that ever changes,
// this assertion is the thing that should fail.
chainParams.push({ key: "b", name: "B", type: "int", min: 0, max: 9 });
if (fp() !== first) {
    console.log("FAIL: the fingerprint is not memoised on identity");
    process.exit(1);
}

// A genuinely new object -- what a re-read produces -- must be hashed again.
const reread = JSON.parse(JSON.stringify(chainParams));
const after = planPages({ hierarchy, chainParams: reread }).fingerprint;
if (after === first) {
    console.log("FAIL: a changed contract kept the old fingerprint");
    process.exit(1);
}

// Note there is no assertion that the ORIGINAL objects still answer their old
// value: the memo holds one entry, so asking again re-hashes them -- and they
// were mutated above, so they now legitimately hash to what the copy did.

console.log("PASS: the fingerprint is memoised on identity and re-hashed on a new contract");
'
