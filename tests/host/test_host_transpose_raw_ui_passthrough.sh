#!/usr/bin/env bash
set -euo pipefail

# Test: raw-ui modules must receive Shift+Up/Down (CC 55/54) untouched.
# Host transpose shortcuts should be disabled while a raw-ui module is loaded.

file="src/move_anything.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -q "int process_host_midi\\(" "$file"; then
  echo "FAIL: Could not locate process_host_midi() in ${file}" >&2
  exit 1
fi

if ! rg -q "raw_ui_module_active" "$file"; then
  echo "FAIL: Missing raw_ui_module_active guard in process_host_midi()" >&2
  exit 1
fi

transpose_start=$(rg -n "Shift \\+ Up/Down = Semitone transpose" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${transpose_start}" ]; then
  echo "FAIL: Could not locate transpose shortcut block in ${file}" >&2
  exit 1
fi

transpose_block=$(sed -n "${transpose_start},$((transpose_start + 24))p" "$file")
if ! echo "${transpose_block}" | rg -q "if \\(!raw_ui_module_active && host_shift_held && value == 127\\)"; then
  echo "FAIL: Transpose shortcut is not guarded for raw-ui modules" >&2
  exit 1
fi

echo "PASS: Host transpose shortcut is disabled for raw-ui modules"
exit 0
