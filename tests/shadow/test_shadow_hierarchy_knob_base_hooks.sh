#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! rg -q 'const baseVal = getSlotParam\(ctx.slot, `\$\{ctx.fullKey\}:base`\);' "$file"; then
  echo "FAIL: hierarchy knob path does not read base value via :base suffix" >&2
  exit 1
fi

if ! rg -q 'const currentVal = \(baseVal !== null\) \? baseVal : getSlotParam\(ctx.slot, ctx.fullKey\);' "$file"; then
  echo "FAIL: hierarchy knob path is not falling back from base to live value" >&2
  exit 1
fi

echo "PASS: hierarchy knob adjustment uses stable base values when modulation is active"
