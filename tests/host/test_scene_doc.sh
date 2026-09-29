#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# src/shared/scene_doc.mjs: a scene is an A/B PAIR over two stored halves, and
# the saved bank. The rules that matter are the ones that LOSE scenes when
# broken: a failed read must never become an empty scope, and a load must
# reach every scope.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi
node --input-type=module -e '
import * as D from "./src/shared/scene_doc.mjs";
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const eq = (what, got, want) => { const a = JSON.stringify(got), b = JSON.stringify(want);
  if (a !== b) fail(what + ": got " + a + ", want " + b); };

eq("16 scenes, 32 snapshots", [D.SCENE_COUNT, D.SNAP_COUNT, D.HALF_COUNT], [16, 16, 32]);
eq("A3 is half 2, B3 half 18", [D.halfA(2), D.halfB(2)], [2, 18]);
const pairs = D.defaultPairs();
eq("scene k starts as Ak + Bk", pairs[5], [5, 5]);
eq("ends: scene 3 = A3 -> B3", D.endsFor(2, pairs), { a: 2, b: 18 });
pairs[2] = [8, 2];
eq("ends: re-paired A9 -> B3", D.endsFor(2, pairs), { a: 8, b: 18 });
pairs[3] = [-1, 4];
eq("ends: just B5 -- A is none, the knobs", D.endsFor(3, pairs), { a: -1, b: 20 });
eq("ends: no active scene, no ends", D.endsFor(-1, pairs), { a: -1, b: -1 });

eq("eight scopes", D.SCOPES.map(s => s.id), ["slot0","slot1","slot2","slot3","mfx","send1","send2","host"]);
eq("the host scope verbs", D.scopeKey(D.SCOPES[7], "dump"), { slot: 0, key: "host:scenes:dump" });
eq("mfx key", D.scopeKey(D.SCOPES[4], "load"), { slot: 0, key: "master_fx:scenes:load" });

eq("null dump is null, not empty", D.parseDump(null), null);
eq("empty dump is empty", D.parseDump(""), []);
eq("half 31 parses", D.parseDump("31 synth cutoff 1 m\n").length, 1);
eq("half 32 is malformed", D.parseDump("32 synth cutoff 1 m\n"), null);

const dumps = { slot0: "4 synth cutoff 0.25 obxd\n5 synth cutoff 0.75 obxd\n", slot1: "", slot2: "", slot3: "",
                mfx: "5 fx1 mix 0.5 cloudseed\n", send1: "", send2: "",
                host: "5 slot2 volume 0.5 host\n" };
const doc = D.buildDoc({ active: 2, pairs, dumps });
eq("doc active + pairs", [doc.active, doc.pairs[2], doc.pairs[3]], [2, [8, 2], [-1, 4]]);
eq("doc groups by half", doc.halves.map(h => [h.n, h.locks.length]), [[4, 1], [5, 3]]);
eq("a missing scope makes NO document", D.buildDoc({ active: 2, pairs, dumps: { ...dumps, send2: null } }), null);

const back = D.parseDoc(JSON.stringify(doc));
eq("round trip keeps active and pairs", [back.active, back.pairs[2], back.pairs[3]], [2, [8, 2], [-1, 4]]);
const loads = D.docToLoads(back);
eq("round trip: slot0", loads.slot0, dumps.slot0);
eq("round trip: host", loads.host, dumps.host);
eq("round trip: an unused scope loads EMPTY", loads.send1, "");
eq("v1/v2 (never shipped) read as an empty bank with default pairs, not files to protect",
   [D.parseDoc(JSON.stringify({ v: 2, halves: [{n: 4}] })).halves.length, D.parseDoc(JSON.stringify({ v: 1 })).legacy,
    D.parseDoc(JSON.stringify({ v: 2 })).pairs[7]], [0, true, [7, 7]]);
eq("an unknown later version is refused", D.parseDoc(JSON.stringify({ v: 4, halves: [] })), null);
eq("a junk pair entry is none", D.parseDoc(JSON.stringify({ v: 3, halves: [], pairs: [[99, "x"]] })).pairs[0], [-1, -1]);
eq("sum counts over 32 halves", D.sumLockCounts(["1" + ",0".repeat(31), "0,2" + ",0".repeat(30)]).slice(0, 2), [1, 2]);
eq("sum counts: a 16-wide answer is not a half answer", D.sumLockCounts(["0" + ",0".repeat(15)]), null);

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: scene_doc");
'
