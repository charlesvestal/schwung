#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# 2026-06 cleanup step 9 (U-4): the slot-preset (settings.mjs) and
# master-preset (master_fx.mjs) overwrite/delete confirmations each
# hand-rendered an identical "header + quoted name + No/Yes selector +
# Back:cancel" modal — four copies of the same widget. Unified onto a
# single drawConfirmModal helper in menu_layout.mjs (pure rendering dedup;
# the confirmIndex/masterConfirmIndex state machines are untouched).

layout="src/shared/menu_layout.mjs"
settings="src/shadow/shadow_ui_settings.mjs"
master="src/shadow/shadow_ui_master_fx.mjs"

# 1. Shared widget exists.
if ! rg -q 'export function drawConfirmModal' "$layout"; then
  echo "FAIL: menu_layout.mjs does not export drawConfirmModal" >&2
  exit 1
fi

# 2. The four confirm draws delegate to it (>= 4 call sites across the two
#    draw modules).
calls=$( { rg -c 'drawConfirmModal\(' "$settings" "$master" 2>/dev/null || true; } | awk -F: '{s+=$2} END {print s+0}')
if [ "$calls" -lt 4 ]; then
  echo "FAIL: expected >=4 drawConfirmModal call sites, found $calls" >&2
  exit 1
fi

# 3. The inline No/Yes selector duplication is gone from the draw modules.
if rg -q 'i === 0 \? "No" : "Yes"' "$settings" "$master"; then
  echo "FAIL: inline No/Yes selector still hand-rendered in a draw module:" >&2
  rg -n 'i === 0 \? "No" : "Yes"' "$settings" "$master" >&2
  exit 1
fi

echo "PASS: confirm modals render via shared drawConfirmModal"
