#!/usr/bin/env bash
set -euo pipefail

shim_file="src/move_anything_shim.c"
ui_file="src/shadow/shadow_ui.js"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# UI should expose a Master FX setting item for bridge mode.
if ! rg -q "key: \"resample_bridge\"" "$ui_file"; then
  echo "FAIL: Master FX settings missing resample_bridge item in shadow_ui.js" >&2
  exit 1
fi

if ! rg -q "master_fx:resample_bridge" "$ui_file"; then
  echo "FAIL: shadow_ui.js does not read/write master_fx:resample_bridge param" >&2
  exit 1
fi

# Shim should handle SET/GET for master_fx:resample_bridge in param request handler.
if ! rg -q "strcmp\\(param_key, \"resample_bridge\"\\)" "$shim_file"; then
  echo "FAIL: Shim missing resample_bridge param handling" >&2
  exit 1
fi

if ! rg -q "native_resample_bridge_mode" "$shim_file"; then
  echo "FAIL: Shim missing native resample bridge mode state" >&2
  exit 1
fi

echo "PASS: Master FX resample bridge setting is wired in UI + shim"
exit 0
