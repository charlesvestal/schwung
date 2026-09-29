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

eq("16 scenes over 32 halves", [D.SCENE_COUNT, D.HALF_COUNT], [16, 32]);
eq("scene 3 is halves 4 and 5", [D.halfA(2), D.halfB(2)], [4, 5]);
const en = D.defaultEnables();
eq("ends: the active scene, both sides on", D.endsFor(2, en), { a: 4, b: 5 });
en[2][0] = false;
eq("ends: A off is none -- the knobs", D.endsFor(2, en), { a: -1, b: 5 });
eq("ends: no active scene, no ends", D.endsFor(-1, en), { a: -1, b: -1 });

eq("seven scopes", D.SCOPES.map(s => s.id), ["slot0","slot1","slot2","slot3","mfx","send1","send2"]);
eq("mfx key", D.scopeKey(D.SCOPES[4], "load"), { slot: 0, key: "master_fx:scenes:load" });

eq("null dump is null, not empty", D.parseDump(null), null);
eq("empty dump is empty", D.parseDump(""), []);
eq("half 31 parses", D.parseDump("31 synth cutoff 1 m\n").length, 1);
eq("half 32 is malformed", D.parseDump("32 synth cutoff 1 m\n"), null);

const dumps = { slot0: "4 synth cutoff 0.25 obxd\n5 synth cutoff 0.75 obxd\n", slot1: "", slot2: "", slot3: "",
                mfx: "5 fx1 mix 0.5 cloudseed\n", send1: "", send2: "" };
const doc = D.buildDoc({ active: 2, enables: en, dumps });
eq("doc active + enables", [doc.active, doc.enables[2]], [2, [false, true]]);
eq("doc groups by half", doc.halves.map(h => [h.n, h.locks.length]), [[4, 1], [5, 2]]);
eq("a missing scope makes NO document", D.buildDoc({ active: 2, enables: en, dumps: { ...dumps, send2: null } }), null);

const back = D.parseDoc(JSON.stringify(doc));
eq("round trip keeps active and enables", [back.active, back.enables[2]], [2, [false, true]]);
const loads = D.docToLoads(back);
eq("round trip: slot0", loads.slot0, dumps.slot0);
eq("round trip: an unused scope loads EMPTY", loads.send1, "");
eq("v1 (never shipped) reads as an empty bank, not a file to protect",
   [D.parseDoc(JSON.stringify({ v: 1, a: 0, b: 0, scenes: [] })).halves.length, D.parseDoc(JSON.stringify({ v: 1 })).legacy], [0, true]);
eq("an unknown later version is refused", D.parseDoc(JSON.stringify({ v: 3, halves: [] })), null);
eq("sum counts over 32 halves", D.sumLockCounts(["1" + ",0".repeat(31), "0,2" + ",0".repeat(30)]).slice(0, 2), [1, 2]);
eq("sum counts: a 16-wide answer is not a half answer", D.sumLockCounts(["0" + ",0".repeat(15)]), null);

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: scene_doc");
'
