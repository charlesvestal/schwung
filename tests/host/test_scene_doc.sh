#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# src/shared/scene_doc.mjs: the saved bank and where each part of it lives.
# The rules that matter are the ones that LOSE scenes when broken: a failed
# read must never become an empty scope, and a load must reach every scope.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi
node --input-type=module -e '
import * as D from "./src/shared/scene_doc.mjs";
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const eq = (what, got, want) => { const a = JSON.stringify(got), b = JSON.stringify(want);
  if (a !== b) fail(what + ": got " + a + ", want " + b); };

eq("seven scopes", D.SCOPES.map(s => s.id), ["slot0","slot1","slot2","slot3","mfx","send1","send2"]);
eq("slot key", D.scopeKey(D.SCOPES[2], "dump"), { slot: 2, key: "scenes:dump" });
eq("mfx key", D.scopeKey(D.SCOPES[4], "load"), { slot: 0, key: "master_fx:scenes:load" });
eq("send key", D.scopeKey(D.SCOPES[6], "count"), { slot: 0, key: "send2:scenes:count" });

eq("null dump is null, not empty", D.parseDump(null), null);
eq("empty dump is empty", D.parseDump(""), []);
eq("dump parses", D.parseDump("0 synth cutoff 0.25 obxd\n15 fx2 mix 1 cloud\n"),
   [{ n: 0, target: "synth", param: "cutoff", value: 0.25, module: "obxd" },
    { n: 15, target: "fx2", param: "mix", value: 1, module: "cloud" }]);
eq("a malformed dump is null", D.parseDump("0 synth cutoff\n"), null);
eq("scene 16 is malformed", D.parseDump("16 synth cutoff 1 m\n"), null);

const dumps = { slot0: "0 synth cutoff 0.25 obxd\n1 synth cutoff 0.75 obxd\n", slot1: "", slot2: "", slot3: "",
                mfx: "1 fx1 mix 0.5 cloudseed\n", send1: "", send2: "" };
const doc = D.buildDoc({ a: 0, b: 1, dumps });
eq("doc a/b", [doc.a, doc.b], [0, 1]);
eq("doc groups by scene", doc.scenes.map(s => [s.n, s.locks.length]), [[0, 1], [1, 2]]);
eq("a missing scope makes NO document", D.buildDoc({ a: 0, b: 1, dumps: { ...dumps, send2: null } }), null);
eq("a garbled scope makes NO document", D.buildDoc({ a: 0, b: 1, dumps: { ...dumps, mfx: "x" } }), null);
eq("out-of-range A saves as none", D.buildDoc({ a: 99, b: -1, dumps }).a, -1);

const text = JSON.stringify(doc);
const back = D.parseDoc(text);
const loads = D.docToLoads(back);
eq("round trip: every scope gets a load", Object.keys(loads), ["slot0","slot1","slot2","slot3","mfx","send1","send2"]);
eq("round trip: slot0", loads.slot0, dumps.slot0);
eq("round trip: mfx", loads.mfx, dumps.mfx);
eq("round trip: an unused scope loads EMPTY (clearing the last set)", loads.send1, "");
eq("expected count", D.expectedPairCount(loads.slot0), 1);
eq("unknown version refused", D.parseDoc(JSON.stringify({ ...doc, v: 2 })), null);
eq("garbage refused", D.parseDoc("{"), null);
eq("a lock with a space in a key is dropped, not loaded",
   D.docToLoads({ v: 1, scenes: [{ n: 0, locks: [{ scope: "slot0", target: "synth", param: "a b", module: "m", value: 1 }] }] }).slot0, "");
eq("sum counts", D.sumLockCounts(["1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2", "1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0"]).slice(0, 2), [2, 1]);
eq("sum counts: any null is null", D.sumLockCounts(["1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2", null]), null);

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: scene_doc");
'
