#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! command -v perl >/dev/null 2>&1; then
  echo "perl is required to run this test" >&2
  exit 1
fi

# Setting midi_fx1:sync should immediately re-check and display MIDI FX warnings.
if ! perl -0ne 'exit((/function\s+setSlotParam\(slot,\s*key,\s*value\)\s*\{[\s\S]*?if\s*\(key\s*===\s*"midi_fx1:sync"\)\s*\{[\s\S]*?checkAndShowMidiFxError\(slot\);[\s\S]*?\}[\s\S]*?\}/s) ? 0 : 1)' "$file"; then
  echo "FAIL: setSlotParam() does not trigger MIDI FX sync warning refresh" >&2
  exit 1
fi

echo "PASS: MIDI FX sync warning refresh wiring present"

# MIDI FX warnings should not be auto-triggered by slot focus changes.
if perl -0ne 'exit((/function\s+updateFocusedSlot\s*\([^)]*\)\s*\{[\s\S]*?checkAndShowMidiFxError\(/s) ? 0 : 1)' "$file"; then
  echo "FAIL: updateFocusedSlot() still triggers MIDI FX warnings" >&2
  exit 1
fi

echo "PASS: slot focus does not auto-trigger MIDI FX warnings"

# MIDI FX warnings should not be auto-triggered when entering module details.
if perl -0ne 'exit((/function\s+enterHierarchyEditor\s*\([^)]*\)\s*\{[\s\S]*?checkAndShowMidiFxError\(/s) ? 0 : 1)' "$file"; then
  echo "FAIL: enterHierarchyEditor() still triggers MIDI FX warnings" >&2
  exit 1
fi

echo "PASS: module details do not auto-trigger MIDI FX warnings"

# Warning overlay should not auto-dismiss on main/parameter knob turns.
if ! perl -0ne 'exit((/assetWarningActive[\s\S]*isMainKnobTurn[\s\S]*MoveMainKnob[\s\S]*isParamKnobTurn[\s\S]*KNOB_CC_START[\s\S]*KNOB_CC_END[\s\S]*if\s*\(!isMainKnobTurn\s*&&\s*!isParamKnobTurn\)/s) ? 0 : 1)' "$file"; then
  echo "FAIL: warning overlay dismissal is not guarded against knob turns" >&2
  exit 1
fi

echo "PASS: warning overlay dismissal ignores knob turns"
