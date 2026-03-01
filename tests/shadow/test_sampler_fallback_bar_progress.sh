#!/usr/bin/env bash
set -euo pipefail

# Regression test: in fallback timing (no MIDI clock), sampler bar progress
# must still advance for the "Bar X / Y" recording UI.

file="src/host/shadow_sampler.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Ensure fallback timeout path exists.
if ! rg -q "sampler_fallback_blocks\\+\\+" "$file"; then
  echo "FAIL: sampler fallback block counter missing" >&2
  exit 1
fi

# Ensure fallback path computes completed bars and updates bar progress.
if ! rg -q "int completed = \\(sampler_fallback_blocks \\* bars\\) / sampler_fallback_target;" "$file"; then
  echo "FAIL: fallback timing does not compute completed bars" >&2
  exit 1
fi

if ! rg -q "sampler_bars_completed = completed;" "$file"; then
  echo "FAIL: fallback timing does not update sampler_bars_completed" >&2
  exit 1
fi

echo "PASS: fallback timing updates sampler bar progress"
exit 0
