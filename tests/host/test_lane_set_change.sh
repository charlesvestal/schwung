#!/usr/bin/env bash
# A set change must not leave the OUTGOING set's lanes in the chain.
#
# Found on hardware: Set 1's p-lock was still in `lanes:state` while the worker
# reported Set 2. `restoreSlotLanes` returned early when the incoming set had
# no lane file, reasoning that an empty `lanes:state` is a no-op -- true of the
# parser, false of the SLOT. The lane is silent (its fingerprint cannot match
# the new set's clip) but still THERE, and the autosave pass writes what the
# slot serves, so it would be written into the incoming set's lanes_<i>.json.
#
# The functions are LIFTED OUT of shadow_ui.js and run under node with stubs,
# rather than grepped for: a grep pin cannot see whether the early return is
# still there, and this project has already paid for pins that were blind to
# the thing they claimed to check.
set -euo pipefail
cd "$(dirname "$0")/../.."
src="src/shadow/shadow_ui.js"
out="build/tests/host/lane_set_change.mjs"
mkdir -p "$(dirname "$out")"

python3 - "$src" "$out" <<'PY'
import re, sys
src, out = sys.argv[1], sys.argv[2]
s = open(src).read()
def fn(name):
    i = s.index("function %s(" % name)
    depth = 0; j = s.index("{", i)
    k = j
    while True:
        if s[k] == "{": depth += 1
        elif s[k] == "}":
            depth -= 1
            if depth == 0: break
        k += 1
    return s[i:k+1]
body = fn("clearSlotLanesQuietly") + "\n\n" + fn("restoreSlotLanes")
open(out, "w").write("export const src = %r;\n" % body)
PY

cat > "${out%.mjs}_run.mjs" <<'JS'
import { src } from "./lane_set_change.mjs";

let calls = [];
let files = {};
const lastWrittenLaneJson = [null, null, null, null];
const g = {
  host_file_exists: (p) => Object.prototype.hasOwnProperty.call(files, p),
  host_read_file: (p) => files[p],
  setSlotParam: (i, k, v) => calls.push([i, k, v]),
  lanePathForSlot: (i) => "/state/lanes_" + i + ".json",
  debugLog: () => {},
  lastWrittenLaneJson,
};
const fn = new Function(...Object.keys(g), src + "; return { restoreSlotLanes, clearSlotLanesQuietly };");
const api = fn(...Object.values(g));

let fails = 0;
const check = (c, m) => { if (!c) { console.log("FAIL: " + m); fails++; } };

/* 1. NO FILE: the slot must be CLEARED, not left alone. */
calls = []; files = {};
api.restoreSlotLanes(1);
check(calls.length === 1 && calls[0][1] === "lanes:clear" && calls[0][2] === "1",
      "an absent lane file must clear the slot, got " + JSON.stringify(calls));
check(lastWrittenLaneJson[1] === null, "the autosave cache was not invalidated");

/* 2. EMPTY FILE: same -- a zero-length document is an absent one. */
calls = []; files = { "/state/lanes_2.json": "" };
api.restoreSlotLanes(2);
check(calls.length === 1 && calls[0][1] === "lanes:clear",
      "an empty lane file must clear the slot, got " + JSON.stringify(calls));

/* 3. A REAL DOCUMENT is handed to the slot verbatim, and the cache remembers
 *    it so the next autosave can skip the write. */
const doc = "V 2\nL synth cutoff 0 2 8 12 3 60 1\nP 9 0.5 1\n";
calls = []; files = { "/state/lanes_0.json": doc };
api.restoreSlotLanes(0);
check(calls.length === 1 && calls[0][1] === "lanes:state" && calls[0][2] === doc,
      "a lane document was not restored verbatim, got " + JSON.stringify(calls));
check(lastWrittenLaneJson[0] === doc,
      "the autosave cache does not match what was handed to the slot");

if (fails) { console.log(fails + " failure(s)"); process.exit(1); }
console.log("PASS: a set change clears lanes the incoming set does not have");
JS
node "${out%.mjs}_run.mjs"
