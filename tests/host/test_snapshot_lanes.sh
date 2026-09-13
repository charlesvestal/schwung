#!/usr/bin/env bash
# Shift+Copy / Shift+Delete must take the AUTOMATION with the sound.
#
# The snapshot was twelve files -- four slots and eight Master FX -- and a lane
# is slot state written by the same autosave pass into the same directory. So a
# recall restored a slot's sound and left whatever automation happened to be
# live: half the state came back.
#
# The file list and the recall's lane loop are LIFTED OUT and run under node,
# because a grep can see the name in the list and not whether anything reads it
# back.
set -euo pipefail
cd "$(dirname "$0")/../.."
src="src/shadow/shadow_ui.js"
out="build/tests/host/snapshot_lanes.mjs"
mkdir -p "$(dirname "$out")"

python3 - "$src" "$out" <<'PY'
import sys, re
src, out = sys.argv[1], sys.argv[2]
s = open(src).read()
def fn(name):
    i = s.index("function %s(" % name)
    depth = 0; k = s.index("{", i)
    while True:
        if s[k] == "{": depth += 1
        elif s[k] == "}":
            depth -= 1
            if depth == 0: break
        k += 1
    return s[i:k+1]
# the lane half of snapshotRecall, by its comment anchor
rec = s.index("    /* THE LANES, per slot and independently of the component plan above.")
end = s.index("    debugLog(\"snapshot: restored \"", rec)
lane_loop = s[rec:end]
body = ("const SHADOW_UI_SLOTS = 4;\nconst MASTER_FX_SLOTS = 8;\n"
        + fn("snapshotFileNames")
        + "\nfunction recallLanes(dir) {\n" + lane_loop + "\n}\n")
open(out, "w").write("export const src = %r;\n" % body)
PY

cat > "${out%.mjs}_run.mjs" <<'JS'
import { src } from "./snapshot_lanes.mjs";

let calls = [];
let files = {};
const lastWrittenLaneJson = [null, null, null, null];
const g = {
  host_read_file: (p) => files[p],
  setSlotParam: (i, k, v) => calls.push([i, k, v]),
  clearSlotLanesQuietly: (i) => { calls.push([i, "lanes:clear", "1"]); lastWrittenLaneJson[i] = null; },
  lastWrittenLaneJson,
};
const api = new Function(...Object.keys(g), src + "; return { snapshotFileNames, recallLanes };")(...Object.values(g));

let fails = 0;
const check = (c, m) => { if (!c) { console.log("FAIL: " + m); fails++; } };

/* 1. The lane files are IN the snapshot, one per slot, alongside the rest. */
const names = api.snapshotFileNames();
for (let i = 0; i < 4; i++) {
  check(names.includes("/lanes_" + i + ".json"), "lanes_" + i + ".json is not in the snapshot");
  check(names.includes("/slot_" + i + ".json"), "slot_" + i + ".json went missing");
}
check(names.filter(n => n.startsWith("/master_fx_")).length === 8,
      "the eight Master FX files went missing");

/* 2. A recall RESTORES a lane document verbatim, and remembers it so the next
 *    autosave does not rewrite what it just read. */
calls = [];
files = { "/snap/lanes_1.json": "V 2\nL synth cutoff 0 2 8 12 3 60 1\nP 9 0.5 1\n" };
api.recallLanes("/snap");
const w = calls.filter(c => c[1] === "lanes:state");
check(w.length === 1 && w[0][0] === 1 && w[0][2] === files["/snap/lanes_1.json"],
      "the lane document was not restored verbatim: " + JSON.stringify(w));
check(lastWrittenLaneJson[1] === files["/snap/lanes_1.json"],
      "the autosave cache does not match what was restored");

/* 3. AND AN ABSENT FILE CLEARS -- the half that makes this an A/B rather than
 *    an accumulation. A snapshot taken before any automation existed must take
 *    the automation away when recalled. */
const cleared = calls.filter(c => c[1] === "lanes:clear").map(c => c[0]);
check(cleared.length === 3 && cleared.includes(0) && cleared.includes(2) && cleared.includes(3),
      "slots with no lane file in the snapshot were not cleared: " + JSON.stringify(cleared));

/* 4. An EMPTY file is an absent one. */
calls = [];
files = { "/snap/lanes_0.json": "" };
api.recallLanes("/snap");
check(calls.filter(c => c[1] === "lanes:state").length === 0 &&
      calls.filter(c => c[1] === "lanes:clear").length === 4,
      "an empty lane file was not treated as absent: " + JSON.stringify(calls));

if (fails) { console.log(fails + " failure(s)"); process.exit(1); }
console.log("PASS: a snapshot carries the automation with the sound");
JS
node "${out%.mjs}_run.mjs"
