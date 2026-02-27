#!/usr/bin/env bash
set -euo pipefail

# Regression test: sampler UI can be dismissed with Back and resumed with
# Shift+Sample while sampler state remains active.

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Pre-ioctl sampler filtering should only block jog/back while sampler UI is
# fullscreen. Hidden sampler should not swallow those controls.
if ! rg -q "sampler_state != SAMPLER_IDLE && sampler_fullscreen_active" "$file"; then
  echo "FAIL: sampler filter does not gate jog/back blocking on sampler_fullscreen_active" >&2
  exit 1
fi

# Sampler menu navigation should only consume jog input while fullscreen is shown.
if ! rg -q "sampler_state == SAMPLER_ARMED && sampler_fullscreen_active" "$file"; then
  echo "FAIL: sampler jog handlers are not gated on sampler_fullscreen_active" >&2
  exit 1
fi

# Back should dismiss fullscreen sampler UI while keeping sampler active.
if ! rg -q "Sampler: fullscreen dismissed via Back" "$file"; then
  echo "FAIL: missing Back handler for dismissing fullscreen sampler UI" >&2
  exit 1
fi

# Shift+Sample should resume fullscreen sampler UI when it is hidden.
if ! rg -q "sampler_state != SAMPLER_IDLE && !sampler_fullscreen_active" "$file"; then
  echo "FAIL: missing Shift+Sample resume condition for hidden active sampler" >&2
  exit 1
fi

if ! rg -q "Sampler: fullscreen resumed via Shift\\+Sample" "$file"; then
  echo "FAIL: missing Shift+Sample resume action log" >&2
  exit 1
fi

echo "PASS: sampler Back-dismiss and Shift+Sample resume behavior is present"
exit 0
