#!/usr/bin/env bash
set -euo pipefail

# In shadow mode, Move must still receive volume touch (note 8) so
# track+volume and native volume workflows keep working.

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

filter_start=$(rg -n "Note messages: filter knob touches" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${filter_start}" ]; then
  echo "FAIL: Could not locate note-touch filter block in ${file}" >&2
  exit 1
fi

filter_block=$(sed -n "${filter_start},$((filter_start + 25))p" "$file")

if ! echo "${filter_block}" | rg -q "d1 <= 7 \\|\\| d1 == 9"; then
  echo "FAIL: Volume touch note 8 still appears filtered from Move in shadow mode" >&2
  exit 1
fi

echo "PASS: Shadow filter preserves volume touch note 8 for Move"
exit 0
