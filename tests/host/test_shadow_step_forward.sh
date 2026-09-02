#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

shim="src/schwung_shim.c"
input="src/shared/param_pages/page_input.mjs"
ctl="src/shared/param_pages/page_controller.mjs"
native="src/shadow/shadow_ui_param_pages.mjs"

# The shim forwards CAPTURED step buttons to the shadow UI.
#
# This is the one piece of new host plumbing parameter locks need, and it is
# narrow on purpose. The gesture is "hold a step, turn an encoder", and its two
# halves are handled on opposite sides of the process: the step is a note the
# shim routes to DSP, the encoder is CC 71-78 which the shim gives to the UI
# and never routes to DSP. Only the UI knows which parameter the knob under the
# finger is bound to, so the step has to reach it too.

if ! rg -q 'd1 >= CAPTURE_STEPS_NOTE_MIN && d1 <= CAPTURE_STEPS_NOTE_MAX' "$shim"; then
  echo "FAIL: shim does not forward step buttons to the shadow UI" >&2
  exit 1
fi

# GATED ON CAPTURE. Steps belong to Move's own sequencer; a patch takes them by
# declaring capture:{groups:["steps"]}, which already blocks them from Move.
# Forwarding without that test would be a new claim on the surface rather than
# a listener on one the patch has already made.
if ! rg -n 'd1 >= CAPTURE_STEPS_NOTE_MIN && d1 <= CAPTURE_STEPS_NOTE_MAX' -A2 "$shim" \
     | rg -q 'shadow_focused_captures_note\(d1\)'; then
  echo "FAIL: step forward is not gated on the focused slot capturing steps" >&2
  exit 1
fi

# PADS ARE NOT FORWARDED by this path. They are Move's performance surface and
# have their own pad_block route; widening this to pads would take 32 more
# buttons off Move for a feature that does not use them.
if rg -n 'd1 >= CAPTURE_STEPS_NOTE_MIN' -A3 "$shim" | rg -q 'CAPTURE_PADS_NOTE_MIN'; then
  echo "FAIL: the step forward must not also forward pads" >&2
  exit 1
fi

# The DSP still receives the step. The slot's own sequencer (9W9 has one) reads
# these, so the forward must be an ADDITION, not a diversion.
if ! rg -n 'shadow_ui_midi_publish\(\(type == 0x90\) \? 0x09 : 0x08, status, d1, d2\);' -A6 "$shim" \
     | rg -q 'shadow_focused_captures_note\(d1\)'; then
  echo "FAIL: captured steps must still reach the slot's DSP" >&2
  exit 1
fi

# --- the editor half lives in the SHARED LIBRARY, not in the native binding ---
#
# 9W9 and its ports draw their own grid through createController + decodeInput
# + applyInput, and never enter the native PARAM_PAGES view. An editor written
# into shadow_ui_param_pages.mjs is invisible to every one of them — which is
# exactly what shipped first, and exactly why "hold step 9, turn Snappy" did
# nothing on the device it was built for. The decoder emits the step, the
# controller owns the gesture, and a module-owned grid gets both for free.
if ! rg -q 'type: "step", step: d1 - STEP_NOTE_FIRST, down' "$input"; then
  echo "FAIL: decodeInput does not emit a step intent" >&2
  exit 1
fi
if ! rg -n 'case "step":' -A6 "$input" | rg -q 'controller.onStepButton\(intent.step, intent.down\)'; then
  echo "FAIL: applyInput does not route the step intent to the controller" >&2
  exit 1
fi
if ! rg -q 'function onStepButton\(step, down\)' "$ctl"; then
  echo "FAIL: the controller has no onStepButton" >&2
  exit 1
fi
if rg -q 'LOCK_STEP_NOTE_MIN|writeLockForHeldStep|handleLockStepButton' "$native"; then
  echo "FAIL: the native binding still carries its own lock editor — it must not diverge from the library's" >&2
  exit 1
fi

# EVERY write passes through one chokepoint. A detent, a throttled flush, an
# enum commit: none may reach setParam(fullKey(...)) directly, or that path
# forgets a step is held.
if rg -q 'setParam\(fullKey\(key\), (wire|s\.pendingWrite\[key\])\)' "$ctl"; then
  echo "FAIL: a controller write bypasses writeParam — it can land on the base while a step is held" >&2
  exit 1
fi

# A throttled write is tagged with its step AT ENQUEUE, so a flush after the
# release still goes to the lock. Deciding at flush time is the leak.
if ! rg -q 's.pendingLockStep\[key\] = lockStep' "$ctl"; then
  echo "FAIL: throttled writes are not tagged with the step they were made against" >&2
  exit 1
fi

# Synthesised contracts (slot settings, Master FX) address slot:* and have no
# lanes; a held step must not swallow their edits.
if ! rg -q 'function lockablePrefix\(\)' "$ctl"; then
  echo "FAIL: locks are not restricted to real chain components" >&2
  exit 1
fi

# A held step must not survive leaving the native grid: the note-off goes to
# whichever screen took over.
if ! rg -n 'export function exitParamPages' -A8 "$native" | rg -q 'clearHeldStep'; then
  echo "FAIL: leaving the native grid does not release a held step" >&2
  exit 1
fi

echo "PASS: captured step buttons reach every grid's controller without leaving the DSP"
