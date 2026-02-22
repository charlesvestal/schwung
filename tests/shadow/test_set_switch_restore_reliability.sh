#!/usr/bin/env bash
set -euo pipefail

ui_c="src/shadow/shadow_ui.c"
ui_js="src/shadow/shadow_ui.js"
shim_c="src/move_anything_shim.c"
constants_h="src/host/shadow_constants.h"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# IPC must carry request/response ids so late responses from timed-out requests
# cannot satisfy newer requests.
if ! rg -q "request_id" "$constants_h"; then
  echo "FAIL: shadow_param_t is missing request_id for robust request matching" >&2
  exit 1
fi
if ! rg -q "response_id" "$constants_h"; then
  echo "FAIL: shadow_param_t is missing response_id for robust request matching" >&2
  exit 1
fi

# UI side: must expose timeout-aware setter and match response_id in wait loops.
if ! rg -q "js_shadow_set_param_timeout\\(" "$ui_c"; then
  echo "FAIL: shadow_ui.c is missing js_shadow_set_param_timeout()" >&2
  exit 1
fi
if ! rg -q "shadow_set_param_timeout" "$ui_c"; then
  echo "FAIL: shadow_ui.c does not export shadow_set_param_timeout global" >&2
  exit 1
fi
if ! rg -q "response_id == req_id" "$ui_c"; then
  echo "FAIL: shadow_ui.c does not validate response_id against req_id" >&2
  exit 1
fi

# Shim side: must capture request id and avoid publishing stale responses.
if ! rg -q "uint32_t req_id = shadow_param->request_id" "$shim_c"; then
  echo "FAIL: shim does not capture shadow_param request_id before processing" >&2
  exit 1
fi
if ! rg -q "if \\(shadow_param->request_id != req_id\\)" "$shim_c"; then
  echo "FAIL: shim does not guard against stale/overwritten requests" >&2
  exit 1
fi

# Set-change restore must use timeout-aware load_file with checked success/retry.
if ! rg -q "setSlotParamWithTimeout\\(i, \"load_file\", path" "$ui_js"; then
  echo "FAIL: SET_CHANGED restore does not use timeout-aware load_file" >&2
  exit 1
fi
if ! rg -q "loadOk = setSlotParamWithTimeout\\(i, \"load_file\", path, 1500\\)" "$ui_js"; then
  echo "FAIL: SET_CHANGED restore missing initial extended load_file timeout" >&2
  exit 1
fi
if ! rg -q "if \\(!loadOk\\)" "$ui_js"; then
  echo "FAIL: SET_CHANGED restore does not check load_file success" >&2
  exit 1
fi
if ! rg -q "SET_CHANGED: load_file timeout" "$ui_js"; then
  echo "FAIL: SET_CHANGED restore missing timeout debug logging for load_file failures" >&2
  exit 1
fi

echo "PASS: set-switch restore reliability wiring present"
exit 0
