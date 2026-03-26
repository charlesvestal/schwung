#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! rg -q 'returnView: VIEWS\.MASTER_FX,' "$file"; then
  echo "FAIL: Master FX LFO editor does not return to Master FX view on Back" >&2
  exit 1
fi

if rg -q 'returnView: VIEWS\.MASTER_FX_SETTINGS' "$file"; then
  echo "FAIL: Master FX LFO editor references undefined MASTER_FX_SETTINGS view" >&2
  exit 1
fi

echo "PASS: Master FX LFO Back navigation returns to Master FX view"
