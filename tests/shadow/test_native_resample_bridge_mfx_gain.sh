#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

fn_start=$(rg -n "static void native_resample_bridge_apply_overwrite_makeup\\(const int16_t \\*src," "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${fn_start}" ]; then
  echo "FAIL: Could not locate overwrite makeup helper" >&2
  exit 1
fi

ctx=$(sed -n "${fn_start},$((fn_start + 150))p" "$file")

# MFX-active overwrite path must compensate for Move volume attenuation.
if ! echo "${ctx}" | rg -q "else if \\(shadow_master_fx_chain_active\\(\\)\\)"; then
  echo "FAIL: Missing dedicated MFX-active compensation branch in overwrite helper" >&2
  exit 1
fi

if ! echo "${ctx}" | rg -q "float inv_mv = 1\\.0f / mv;"; then
  echo "FAIL: Missing inverse master-volume gain computation in overwrite helper" >&2
  exit 1
fi

if ! echo "${ctx}" | rg -q "\\(float\\)src\\[i\\] \\* applied_gain"; then
  echo "FAIL: MFX-active branch should scale snapshot samples by applied gain" >&2
  exit 1
fi

if ! echo "${ctx}" | rg -q "native_bridge_makeup_desired_gain = inv_mv;"; then
  echo "FAIL: MFX-active branch should report desired gain in diagnostics" >&2
  exit 1
fi

echo "PASS: Native resample bridge MFX compensation wiring present"
exit 0
