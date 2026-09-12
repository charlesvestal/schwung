#!/usr/bin/env bash
# The p-lock gesture's JS half: which step is held, and what gets written.
#
# The functions are LIFTED OUT of shadow_ui.js and run under node with stubs.
# A grep pin could not see the thing that matters here -- that a release drops
# the hold, that two held steps resolve to one, and that the write carries the
# value the KNOB produced rather than a value the gesture invented.
set -euo pipefail
cd "$(dirname "$0")/../.."
src="src/shadow/shadow_ui.js"
out="build/tests/host/plock_gesture.mjs"
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
# the module-level state the three functions share
state = "const stepHeld = [];\nlet stepHeldLast = -1;\n"
body = state + "\n".join(fn(n) for n in
                         ("noteStepIndex", "onStepNote", "heldStepIndex"))
open(out, "w").write("export const src = %r;\n" % body)
PY

cat > "${out%.mjs}_run.mjs" <<'JS'
import { src } from "./plock_gesture.mjs";
const api = new Function(src + "; return { noteStepIndex, onStepNote, heldStepIndex };")();

let fails = 0;
const check = (c, m) => { if (!c) { console.log("FAIL: " + m); fails++; } };

/* 1. Only the sixteen step notes are steps. Pads (68-99) and knob touch (0-9)
 *    are NOT: a p-lock fired from a pad would be a value on a step nobody
 *    pressed. */
check(api.noteStepIndex(16) === 0 && api.noteStepIndex(31) === 15,
      "steps 16..31 map to 0..15");
check(api.noteStepIndex(15) < 0 && api.noteStepIndex(32) < 0 &&
      api.noteStepIndex(68) < 0 && api.noteStepIndex(0) < 0,
      "a note outside 16..31 is not a step");

/* 2. Press and release. */
check(api.heldStepIndex() === -1, "nothing held to start");
api.onStepNote(20, 127);
check(api.heldStepIndex() === 4, "step 20 held -> index 4");
api.onStepNote(20, 0);
check(api.heldStepIndex() === -1, "released");

/* 3. A note-on with velocity 0 IS a release. Move does not send one today,
 *    but every other note path in this file treats it as one, and a gesture
 *    that disagreed would leave a step held forever. */
api.onStepNote(18, 127);
api.onStepNote(18, 0);
check(api.heldStepIndex() === -1, "velocity 0 is a release");

/* 4. TWO HELD STEPS resolve to the LAST pressed -- two fingers down is not a
 *    gesture anyone can mean, and taking the earlier one makes the second
 *    press feel dead. Releasing it falls back to the one still down rather
 *    than to nothing. */
api.onStepNote(16, 127);   /* index 0 */
api.onStepNote(24, 127);   /* index 8, later */
check(api.heldStepIndex() === 8, "the later press wins");
api.onStepNote(24, 0);
check(api.heldStepIndex() === 0,
      "releasing the later one falls back to the one still held, got " +
      api.heldStepIndex());
api.onStepNote(16, 0);
check(api.heldStepIndex() === -1, "both released");

/* 5. A release for a step that was never pressed changes nothing. The shim
 *    can drop step_observe mid-gesture, so the UI sees releases with no
 *    matching press. */
api.onStepNote(28, 0);
check(api.heldStepIndex() === -1, "an orphan release held nothing");

if (fails) { console.log(fails + " failure(s)"); process.exit(1); }
console.log("PASS: p-lock gesture, held-step tracking");
JS
node "${out%.mjs}_run.mjs"
