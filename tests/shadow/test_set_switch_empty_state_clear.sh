#!/usr/bin/env bash
set -euo pipefail

ui_js="src/shadow/shadow_ui.js"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# New/empty set states must clear slots (so fresh sets start empty).
if ! rg -q "SET_CHANGED: clearing slot" "$ui_js"; then
  echo "FAIL: SET_CHANGED restore has no explicit clear path for empty slot state" >&2
  exit 1
fi
if ! rg -q "function clearSlotForEmptySetState\(slot\)" "$ui_js"; then
  echo "FAIL: SET_CHANGED empty-state clear helper missing" >&2
  exit 1
fi
if ! rg -q "\"synth:module\", \"midi_fx1:module\", \"fx1:module\", \"fx2:module\"" "$ui_js"; then
  echo "FAIL: SET_CHANGED empty-state clear helper missing required module keys" >&2
  exit 1
fi
if ! rg -q "setSlotParamWithRetry\(slot, key, \"\", 1500, 3000, \"SET_CHANGED: clear\"\)" "$ui_js"; then
  echo "FAIL: SET_CHANGED empty-state clear missing timeout/retry wiring" >&2
  exit 1
fi
if ! rg -q "clearSlotForEmptySetState\(i\)" "$ui_js"; then
  echo "FAIL: SET_CHANGED path does not use empty-state clear helper" >&2
  exit 1
fi
if ! rg -q "\\$\\{logLabel\\} timeout slot" "$ui_js"; then
  echo "FAIL: SET_CHANGED clear path missing timeout logging" >&2
  exit 1
fi

# Timeout failures on non-empty files should preserve current DSP state.
if ! rg -q "not restored \(load timeout\), preserving current DSP state" "$ui_js"; then
  echo "FAIL: SET_CHANGED timeout-preserve behavior missing" >&2
  exit 1
fi

# First-visit set dirs should be explicitly ensured before writes.
if ! rg -q "host_ensure_dir\(newDir\)" "$ui_js"; then
  echo "FAIL: SET_CHANGED does not ensure per-set state directory exists" >&2
  exit 1
fi

echo "PASS: SET_CHANGED empty-state clear and timeout-preserve wiring present"
exit 0
