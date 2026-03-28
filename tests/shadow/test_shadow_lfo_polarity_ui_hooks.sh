#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! rg -F -q '{ key: "polarity", label: "Mode", type: "enum", options: ["Unipolar", "Bipolar"] }' "$file"; then
  echo "FAIL: LFO editor is missing polarity mode menu item" >&2
  exit 1
fi

if ! rg -F -q 'items.push({ key: "depth", label: "Depth", type: "float", min: -1, max: 1, step: 0.01, unit: "%" });' "$file"; then
  echo "FAIL: LFO depth menu item does not support negative range" >&2
  exit 1
fi

if ! rg -q 'if \(item.key === "polarity"\) return raw === "1" \? "Bipolar" : "Unipolar";' "$file"; then
  echo "FAIL: LFO polarity display formatting is missing" >&2
  exit 1
fi

echo "PASS: shadow LFO editor exposes polarity mode and negative depth range"
