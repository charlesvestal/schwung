#!/usr/bin/env bash
set -euo pipefail

# In overtake menu mode (1), volume touch/turn should still reach Move so
# native volume/track-volume overlays work. In overtake module mode (2),
# cable 0 input should remain blocked from Move.

file="src/move_anything_shim.c"

if ! command -v perl >/dev/null 2>&1; then
  echo "perl is required to run this test" >&2
  exit 1
fi

if ! perl -0ne 'exit((/if\s*\(overtake_mode\s*==\s*2\)\s*\{[\s\S]*?filter\s*=\s*1[\s\S]*?\}/s && /if\s*\(overtake_mode\s*==\s*1\)\s*\{[\s\S]*?CC_MASTER_KNOB[\s\S]*?d1\s*==\s*8[\s\S]*?filter\s*=\s*1[\s\S]*?\}/s) ? 0 : 1)' "$file"; then
  echo "FAIL: Overtake menu volume passthrough split (mode 1 pass, mode 2 block) missing in ${file}" >&2
  exit 1
fi

echo "PASS: Overtake menu keeps volume passthrough while overtake module mode blocks cable 0"
exit 0
