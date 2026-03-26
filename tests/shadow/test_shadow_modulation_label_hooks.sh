#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! rg -q 'function getHierarchyDisplayRawValue\(slot, fullKey\)' "$file"; then
  echo "FAIL: hierarchy display base-value helper is missing" >&2
  exit 1
fi

if ! rg -q 'const baseVal = getSlotParam\(slot, `\$\{fullKey\}:base`\);' "$file"; then
  echo "FAIL: hierarchy display helper does not read :base" >&2
  exit 1
fi

if ! rg -q 'function isHierarchyParamModulated\(slot, fullKey\)' "$file"; then
  echo "FAIL: hierarchy modulation status helper is missing" >&2
  exit 1
fi

if ! rg -q 'const modulated = getSlotParam\(slot, `\$\{fullKey\}:modulated`\);' "$file"; then
  echo "FAIL: hierarchy modulation helper does not read :modulated flag" >&2
  exit 1
fi

if ! rg -q 'displayLabel = `\$\{label\}~`;' "$file"; then
  echo "FAIL: hierarchy list does not append ~ to modulated labels" >&2
  exit 1
fi

if ! rg -q 'const title = isHierarchyParamModulated\(ctx.slot, ctx.fullKey\) \? `\$\{ctx.title\}~` : ctx.title;' "$file"; then
  echo "FAIL: knob overlay title does not mark modulated params" >&2
  exit 1
fi

echo "PASS: hierarchy UI uses base values and marks modulated params with ~"
