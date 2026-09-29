#!/usr/bin/env bash
# The UI half of set alignment (see test_set_alignment.sh for the shim half).
#
# 1. THE `set_aligned` ACK IS VERIFIED AND RESENT. It was sent once, result
#    ignored, behind a set change's dozens of slot writes. One lost write left
#    set_doc_gen behind for the session and autosave off with it (the shim's
#    same-set path refuses an unacked set; the cleared flag is never raised
#    again). alignAckTick is LIFTED OUT of shadow_ui.js and run under node.
#
# 2. THE SCENE AUTOSAVE IS GATED LIKE THE SLOT AUTOSAVE. scenesTick saved to
#    activeSlotStateDir after its debounce without asking setAlignmentPending(),
#    so a scene edit made after Move loaded a new set -- before Schwung
#    switched -- was written into the OUTGOING set's folder. Also lifted.
#
# 3. THE OUTGOING SCENES ARE SAVED BEFORE THE PENDING-SET MIGRATION. Step 3b
#    moves a pending set's folder (`cp -a ... && rm -rf`) on its first save;
#    the scenes save ran AFTER it, into the deleted folder, and step 8c then
#    loaded the stale copy over the live bank. The handler is ~450 lines of
#    file and IPC work, so this part is an ORDER pin over the source.
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v node >/dev/null || { echo "SKIP: node not available"; exit 0; }
src="src/shadow/shadow_ui.js"
out="build/tests/host/set_alignment_ui"
mkdir -p "$(dirname "$out")"

python3 - "$src" "$out.mjs" <<'PY'
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
def const(name):
    i = s.index("const %s =" % name)
    return s[i:s.index(";", i)+1]
ack = "\n".join([const("ALIGN_ACK_INTERVAL_MS"), const("ALIGN_ACK_TRIES"),
                 "let alignAck = null;", fn("armAlignAck"), fn("alignAckTick")])
tick = fn("scenesTick")
open(out, "w").write("export const ack = %r;\nexport const tick = %r;\n" % (ack, tick))
PY

cat > "${out}_run.mjs" <<'JS'
import { ack, tick } from "./set_alignment_ui.mjs";
let fails = 0;
const check = (c, m) => { if (!c) { console.log("FAIL: " + m); fails++; } };

/* ---- 1. the ack ------------------------------------------------------- */
let state = [1, 5, 4];            /* ready, move_doc_gen, set_doc_gen */
let writes = [];
const g = {
  moveModelState: () => state,
  setSlotParamWithTimeout: (slot, k, v, t) => { writes.push([k, v, t]); return false; },
  debugLog: () => {},
};
const mk = () => new Function(...Object.keys(g), ack +
  "; return { armAlignAck, alignAckTick, get: () => alignAck };")(...Object.values(g));

/* The ack landed: nothing is resent. */
let A = mk();
state = [1, 5, 5]; writes = [];
A.armAlignAck(5, 0);
for (let t = 0; t < 5000; t += 16) A.alignAckTick(t);
check(writes.length === 0, "a confirmed ack is never resent (" + writes.length + ")");
check(A.get() === null, "and the retry disarms");

/* The ack was LOST: resent on an interval, never more than once a tick,
 * until the shim reports it -- then it stops. */
A = mk();
state = [1, 5, 4]; writes = [];
A.armAlignAck(5, 0);
A.alignAckTick(0);
check(writes.length === 0, "nothing is resent before the first interval");
let t = 0;
for (; t < 1600; t += 16) {
  const before = writes.length;
  A.alignAckTick(t);
  check(writes.length - before <= 1, "at most one write per tick");
}
check(writes.length >= 2 && writes.length <= 4, "resent on an interval, got " + writes.length);
check(writes.every((w) => w[0] === "set_aligned" && w[1] === "5"), "it resends the HANDLED generation");
check(writes.every((w) => w[2] <= 100), "each resend is a short write, never a long block");
state = [1, 5, 5];                /* the resend landed */
const n = writes.length;
for (; t < 5000; t += 16) A.alignAckTick(t);
check(writes.length === n && A.get() === null, "stops once the shim reports it");

/* Aligned some other way (a same-set reload in C, the give-up): stop. */
A = mk();
state = [1, 5, 4]; writes = [];
A.armAlignAck(5, 0);
state = [1, 6, 6];
for (let u = 0; u < 3000; u += 16) A.alignAckTick(u);
check(writes.length === 0 && A.get() === null, "an aligned shim needs no ack");

/* Never confirmed: BOUNDED. */
A = mk();
state = [1, 5, 4]; writes = [];
A.armAlignAck(5, 0);
for (let u = 0; u < 120000; u += 16) A.alignAckTick(u);
check(writes.length > 0 && writes.length <= 20, "bounded, got " + writes.length);
check(A.get() === null, "and gives up to the shim's own give-up");

/* ---- 2. the scene autosave ------------------------------------------ */
let pending = false, saves = [], now = 0;
const st = { rev: 1, edit: -1 };
const h = {
  view: 0, VIEWS: { OVERTAKE_MODULE: 9, SCENES: 8 }, scenesScreen: {}, ccMap: {},
  needsRedraw: false,
  sceneState: () => st, sceneSetEdit: () => {}, scenesAdoptPc: () => {},
  sceneLoadConfirmed: true, sceneLoadRefused: false, sceneLoadDir: "/old",
  sceneLoadNextTry: 0, scenesLoadFrom: () => {},
  sceneSaveKey: (x) => String(x.rev), sceneSavedKey: "0",
  SCENE_SAVE_DEBOUNCE_MS: 1000,
  scenesSaveTo: (d) => { saves.push(d); return true; },
  activeSlotStateDir: "/old",
  setAlignmentPending: () => pending,
  Date: { now: () => now },
};
const T = new Function(...Object.keys(h),
  "let sceneDirtyAt = 0;\n" + tick + "; return scenesTick;")(...Object.values(h));
pending = true;
for (now = 0; now < 5000; now += 16) T();
check(saves.length === 0, "a dirty scene bank is not saved while alignment is pending (" + saves.length + ")");
pending = false;
T();
check(saves.length === 1, "and is saved as soon as it is aligned (" + saves.length + ")");

if (fails) { console.log("test_set_alignment_ui: " + fails + " FAILED"); process.exit(1); }
JS
node "${out}_run.mjs"

# ---- 3. the order pin --------------------------------------------------
fail() { echo "FAIL: $1" >&2; exit 1; }
start=$(grep -n 'if (flags & SHADOW_UI_FLAG_SET_CHANGED) setChange: {' "$src" | head -n1 | cut -d: -f1)
[ -n "$start" ] || fail "cannot find the SET_CHANGED handler"
mig=$(awk -v s="$start" 'NR>s && /rm -rf .* from/ { print NR; exit }' "$src")
[ -n "$mig" ] || fail "cannot find the pending-set migration (cp -a ... rm -rf)"
switch=$(awk -v s="$start" 'NR>s && /activeSlotStateDir = newDir;/ { print NR; exit }' "$src")
[ -n "$switch" ] || fail "cannot find the directory switch"
save=$(awk -v s="$start" -v e="$mig" 'NR>s && NR<e && /scenesSaveTo\(activeSlotStateDir\)/ { print NR; exit }' "$src")
[ -n "$save" ] || fail "the outgoing scenes are not saved BEFORE the pending-set migration \
(line $mig) -- a save after it writes into the folder it just deleted"
late=$(awk -v s="$mig" -v e="$switch" 'NR>s && NR<e && /scenesSaveTo\(/ { print NR; exit }' "$src")
[ -z "$late" ] || fail "a scenes save at line $late runs after the migration deleted \
activeSlotStateDir (line $mig) and before the switch (line $switch)"

# ...and the retry is actually wired: armed where the ack is sent, ticked.
ackl=$(awk -v s="$start" 'NR>s && /"set_aligned", String\(handledGen\)/ { print NR; exit }' "$src")
[ -n "$ackl" ] || fail "cannot find the handler's set_aligned write"
sed -n "${ackl},$((ackl + 2))p" "$src" | grep -q 'armAlignAck(handledGen' \
  || fail "the set_aligned write is not followed by armAlignAck(handledGen) -- a lost ack is never resent"
grep -qE '^\s*if \(!isOvertakeActive\) alignAckTick\(Date\.now\(\)\);' "$src" \
  || fail "alignAckTick is not called from tick()"

echo "test_set_alignment_ui: PASS"
