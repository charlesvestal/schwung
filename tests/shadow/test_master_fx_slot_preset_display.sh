#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! command -v perl >/dev/null 2>&1; then
  echo "perl is required to run this test" >&2
  exit 1
fi

# Master FX component detail line should mirror instrument slot behavior by
# appending selected preset text when available.
if ! perl -0ne 'exit((/function\s+drawMasterFx\(\)\s*\{[\s\S]*?const preset = getMasterFxParam\(selectedMasterFxComponent,\s*"preset_name"\)\s*\|\|\s*getMasterFxParam\(selectedMasterFxComponent,\s*"preset"\)\s*\|\|\s*"";[\s\S]*?truncateText\(preset,\s*8\)[\s\S]*?\}/s) ? 0 : 1)' "$file"; then
  echo "FAIL: drawMasterFx() does not include preset_name/preset in Master FX info line (${file})" >&2
  exit 1
fi

echo "PASS: Master FX detail line includes selected preset name"
exit 0
