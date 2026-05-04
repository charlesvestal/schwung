#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
shadow_file="src/shadow/shadow_ui.js"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Imports knob_engine via the absolute on-device path
if ! rg -F -q "from '/data/UserData/schwung/shared/knob_engine.mjs'" "$shadow_file"; then
  echo "FAIL: shadow_ui.js does not import knob_engine.mjs (absolute on-device path)" >&2
  exit 1
fi

# Calls knobTick somewhere in the file
if ! rg -F -q 'knobTick(' "$shadow_file"; then
  echo "FAIL: shadow_ui.js does not call knobTick" >&2
  exit 1
fi

# adjustHierSelectedParam SHOULD use simple linear math (no acceleration)
adjust_body=$(awk '
  /^function adjustHierSelectedParam\(/ { flag=1; print; next }
  flag && /^function / { flag=0 }
  flag { print }
' "$shadow_file")

if ! echo "$adjust_body" | grep -F -q 'num + delta * step'; then
  echo "FAIL: adjustHierSelectedParam should use simple linear math (jog click = one step)" >&2
  exit 1
fi

# processPendingHierKnob (physical knobs 1-8) SHOULD use knob_engine, NOT linear math
phys_body=$(awk '
  /^function processPendingHierKnob\(/ { flag=1; print; next }
  flag && /^function / { flag=0 }
  flag { print }
' "$shadow_file")

if echo "$phys_body" | grep -F -q 'num + delta * step'; then
  echo "FAIL: processPendingHierKnob still uses linear math instead of knob_engine" >&2
  exit 1
fi

if ! echo "$phys_body" | grep -F -q 'knobTick('; then
  echo "FAIL: processPendingHierKnob does not call knobTick" >&2
  exit 1
fi

if ! echo "$phys_body" | grep -F -q 'getPhysKnobState'; then
  echo "FAIL: processPendingHierKnob does not use the physKnobStates cache" >&2
  exit 1
fi

# physKnobStates map and clearer must be defined at top level
if ! rg -F -q 'physKnobStates' "$shadow_file"; then
  echo "FAIL: physKnobStates map missing" >&2
  exit 1
fi

if ! rg -F -q 'clearPhysKnobStates' "$shadow_file"; then
  echo "FAIL: clearPhysKnobStates helper missing" >&2
  exit 1
fi

echo "PASS"
