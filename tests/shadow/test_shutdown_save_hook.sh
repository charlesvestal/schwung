#!/usr/bin/env bash
set -euo pipefail

ui_js="src/shadow/shadow_ui.js"
ui_c="src/shadow/shadow_ui.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -q "globalThis\.shadow_save_state_now\s*=\s*function" "$ui_js"; then
  echo "FAIL: shadow_ui.js is missing global save hook shadow_save_state_now()" >&2
  exit 1
fi
if ! rg -q "shadow_save_state_now" "$ui_c"; then
  echo "FAIL: shadow_ui.c does not look up shadow_save_state_now()" >&2
  exit 1
fi
if ! rg -q "autosaveAllSlots\(\)" "$ui_js"; then
  echo "FAIL: shadow_save_state_now() does not flush slot state" >&2
  exit 1
fi
if ! rg -q "saveMasterFxChainConfig\(\)" "$ui_js"; then
  echo "FAIL: shadow_save_state_now() does not flush master FX state" >&2
  exit 1
fi
if ! rg -q "shadow_control->should_exit" "$ui_c"; then
  echo "FAIL: shadow_ui.c missing should_exit check" >&2
  exit 1
fi
if ! rg -q "callGlobalFunction\(ctx, &JSSaveState, 0\)" "$ui_c"; then
  echo "FAIL: shadow_ui.c does not call save hook before exit" >&2
  exit 1
fi

echo "PASS: shutdown/restart save hook wiring present"
exit 0
