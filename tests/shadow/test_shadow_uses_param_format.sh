#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
shadow_file="src/shadow/shadow_ui.js"
patches_file="src/shadow/shadow_ui_patches.mjs"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Both files import param_format.mjs via the absolute on-device path.
if ! rg -F -q "from '/data/UserData/schwung/shared/param_format.mjs'" "$shadow_file"; then
  echo "FAIL: shadow_ui.js does not import param_format.mjs" >&2
  exit 1
fi

if ! rg -F -q "from '/data/UserData/schwung/shared/param_format.mjs'" "$patches_file"; then
  echo "FAIL: shadow_ui_patches.mjs does not import param_format.mjs" >&2
  exit 1
fi

# patches.mjs also imports knob_engine for the adjust path
if ! rg -F -q "from '/data/UserData/schwung/shared/knob_engine.mjs'" "$patches_file"; then
  echo "FAIL: shadow_ui_patches.mjs does not import knob_engine.mjs" >&2
  exit 1
fi

# Old inline applyDisplayFormat helper must be removed (now lives in param_format.mjs)
inline_count=$(rg -F -c 'function applyDisplayFormat' "$shadow_file" || true)
if [ "${inline_count:-0}" != "0" ]; then
  echo "FAIL: applyDisplayFormat still defined inline in shadow_ui.js" >&2
  exit 1
fi

# Also no dangling references to the unqualified applyDisplayFormat symbol
# (must be imported as ufApplyDisplayFormat or fully-qualified).
dangling=$(rg -F -n 'applyDisplayFormat' "$shadow_file" | rg -v 'ufApplyDisplayFormat' || true)
if [ -n "$dangling" ]; then
  echo "FAIL: dangling reference(s) to applyDisplayFormat in shadow_ui.js:" >&2
  echo "$dangling" >&2
  exit 1
fi

echo "PASS"
