#!/usr/bin/env bash
# The lane autosave skips `lanes:state` while `lanes:rev` says nothing changed.
#
# `lanes:state` makes the chain serialise the whole store ON THE SPI CALLBACK
# (~1 ms on a Mac for a full store, several on the device), for every slot on
# every ~5 s autosave pass, whether or not anything was recorded. The chain now
# serves a content hash as `lanes:rev` (tests/host/test_lane_review_fixes.c,
# case 7), and persistSlotLanes asks that first.
#
# The skip must never cost a save, so the cases that matter are the ones where
# it must NOT skip: a changed rev, a rev that did not answer, a cache that was
# never verified, and the empty branch (which also reports stalled takes).
#
# persistSlotLanes is LIFTED OUT of shadow_ui.js and run under node with
# stubs, rather than grepped for.
set -euo pipefail
cd "$(dirname "$0")/../.."
src="src/shadow/shadow_ui.js"
out="build/tests/host/lane_autosave_rev.mjs"
mkdir -p "$(dirname "$out")"

python3 - "$src" "$out" <<'PY'
import sys
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
open(out, "w").write("export const src = %r;\n" % fn("persistSlotLanes"))
PY

cat > "${out%.mjs}_run.mjs" <<'JS'
import { src } from "./lane_autosave_rev.mjs";

let rev = "r1", doc = "V 2\nL synth cutoff 0 0 0 8 3 60 1\nP 1 50 0\n";
let stateReads = 0, writes = 0, writeOk = true;
const lastWrittenLaneJson = [null, null, null, null];
const lastWrittenLaneRev = [null, null, null, null];
const g = {
  getSlotParam: (i, k) => (k === "lanes:rev" ? rev : (k === "lanes:pending" ? "" : null)),
  getSlotStateWithRetry: (i, k) => { stateReads++; return doc; },
  lanePathForSlot: (i) => "/state/lanes_" + i + ".json",
  host_write_file: () => { writes++; return writeOk; },
  host_file_exists: () => true,
  debugLog: () => {}, announce: () => {},
  laneRestoreConfirmed: [true, true, true, true],
  laneStallAnnounced: [false, false, false, false],
  lastWrittenLaneJson, lastWrittenLaneRev,
};
const persist = new Function(...Object.keys(g), src + "; return persistSlotLanes;")(...Object.values(g));

let fails = 0;
const check = (c, m) => { if (!c) { console.log("FAIL: " + m); fails++; } else console.log("  ok  " + m); };
const pass = () => { stateReads = 0; writes = 0; persist(0); };

pass();
check(stateReads === 1 && writes === 1, "first pass reads and writes");
pass();
check(stateReads === 0 && writes === 0, "an unchanged rev skips the document read entirely");
rev = "r2"; doc = doc.replace("50", "60");
pass();
check(stateReads === 1 && writes === 1, "a changed rev reads and writes");
rev = null;
pass();
check(stateReads === 1, "a rev that did not answer never skips");
rev = "r2";
pass();
check(stateReads === 1 && writes === 0, "a rev not yet verified reads (and finds the file current)");
pass();
check(stateReads === 0, "...and is trusted after that");
rev = "r3"; doc = doc.replace("60", "70"); writeOk = false;
pass();
rev = "r3"; writeOk = true;
pass();
check(stateReads === 1 && writes === 1, "a FAILED write is retried, never skipped on the same rev");
lastWrittenLaneJson[0] = null;
pass();
check(stateReads === 1, "an invalidated cache (restore, recall, set change) reads");
rev = "r4"; doc = "";
pass(); pass();
check(stateReads === 1, "the empty branch is re-read every pass (it reports stalled takes)");

if (fails) { console.log(fails + " failure(s)"); process.exit(1); }
console.log("PASS: the lane autosave skips an unchanged store");
JS
node "${out%.mjs}_run.mjs"
