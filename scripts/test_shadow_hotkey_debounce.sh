#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

line=$(rg -n "shadowModeDebounce && \\(!shiftHeld" "$file" | head -n 1 || true)
if [ -z "${line}" ]; then
  echo "Failed to locate shadowModeDebounce reset condition in ${file}" >&2
  exit 1
fi

if echo "${line}" | rg -q "\\|\\|"; then
  echo "PASS: shadowModeDebounce resets when any hotkey part is released"
  exit 0
fi

echo "FAIL: shadowModeDebounce only resets when all parts are released" >&2
exit 1
