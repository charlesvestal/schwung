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

# Verify adjustHierSelectedParam does NOT use the bare linear math anymore
adjust_body=$(awk '
  /^function adjustHierSelectedParam\(/ { capture=1 }
  capture { print }
  capture && /^}/ { capture=0 }
' "$shadow_file")

if echo "$adjust_body" | grep -F -q 'num + delta * step'; then
  echo "FAIL: linear delta math still present in adjustHierSelectedParam" >&2
  exit 1
fi

# State map should be defined and cleared somewhere
if ! rg -F -q 'hierKnobStates' "$shadow_file"; then
  echo "FAIL: hierKnobStates map missing" >&2
  exit 1
fi

if ! rg -F -q 'clearHierKnobStates' "$shadow_file"; then
  echo "FAIL: clearHierKnobStates helper missing" >&2
  exit 1
fi

echo "PASS"
