#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# 1) Core state and bridge functions must exist.
for sym in \
  "native_sampler_source_t" \
  "static int native_resample_bridge_source_allows_apply\\(native_resample_bridge_mode_t mode\\)" \
  "static void native_resample_bridge_apply_overwrite_makeup\\(const int16_t \\*src," \
  "static void native_resample_bridge_apply\\(void\\)"; do
  if ! rg -q "$sym" "$file"; then
    echo "FAIL: Missing bridge symbol: $sym" >&2
    exit 1
  fi
done

# 2) Apply path must use mode/source gating and overwrite helper.
bridge_start=$(rg -n "static void native_resample_bridge_apply\\(void\\)" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${bridge_start}" ]; then
  echo "FAIL: Could not locate native_resample_bridge_apply()" >&2
  exit 1
fi
bridge_ctx=$(sed -n "${bridge_start},$((bridge_start + 90))p" "$file")

if ! echo "${bridge_ctx}" | rg -q "native_resample_bridge_source_allows_apply\\("; then
  echo "FAIL: Apply path missing source allow helper usage" >&2
  exit 1
fi
if ! echo "${bridge_ctx}" | rg -q "mode == NATIVE_RESAMPLE_BRIDGE_OVERWRITE"; then
  echo "FAIL: Apply path missing overwrite mode branch" >&2
  exit 1
fi
if ! echo "${bridge_ctx}" | rg -q "native_resample_bridge_apply_overwrite_makeup\\("; then
  echo "FAIL: Apply path missing overwrite makeup helper call" >&2
  exit 1
fi

# 3) Overwrite helper must support both no-MFX component comp and MFX post-FX makeup.
overwrite_start=$(rg -n "static void native_resample_bridge_apply_overwrite_makeup\\(const int16_t \\*src," "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${overwrite_start}" ]; then
  echo "FAIL: Could not locate overwrite helper" >&2
  exit 1
fi
overwrite_ctx=$(sed -n "${overwrite_start},$((overwrite_start + 190))p" "$file")

if ! echo "${overwrite_ctx}" | rg -q "!shadow_master_fx_chain_active\\(\\) && native_bridge_split_valid"; then
  echo "FAIL: Missing no-MFX split-component compensation branch" >&2
  exit 1
fi
if ! echo "${overwrite_ctx}" | rg -q "native_bridge_move_component"; then
  echo "FAIL: No-MFX branch must use native move component buffer" >&2
  exit 1
fi
if ! echo "${overwrite_ctx}" | rg -q "native_bridge_me_component"; then
  echo "FAIL: No-MFX branch must use native ME component buffer" >&2
  exit 1
fi
if ! echo "${overwrite_ctx}" | rg -q "else if \\(shadow_master_fx_chain_active\\(\\)\\)"; then
  echo "FAIL: Missing MFX-active makeup branch" >&2
  exit 1
fi
if ! echo "${overwrite_ctx}" | rg -q "float inv_mv = 1\\.0f / mv;"; then
  echo "FAIL: Overwrite helper missing inverse master-volume gain" >&2
  exit 1
fi
if ! echo "${overwrite_ctx}" | rg -q "\\(float\\)src\\[i\\] \\* applied_gain"; then
  echo "FAIL: MFX branch should scale snapshot samples by applied gain" >&2
  exit 1
fi

# 4) Diagnostic log should report split/mfx/makeup for hardware validation.
if ! rg -q "split=%d mfx=%d makeup=\\(%.2fx->%.2fx lim=%d\\)" "$file"; then
  echo "FAIL: Bridge diagnostics missing split/mfx/makeup fields" >&2
  exit 1
fi

# 5) Snapshot capture in mix path should be post-FX and before sampler capture.
mix_start=$(rg -n "static void shadow_inprocess_mix_from_buffer\\(void\\)" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${mix_start}" ]; then
  echo "FAIL: Could not locate shadow_inprocess_mix_from_buffer()" >&2
  exit 1
fi
mix_ctx=$(sed -n "${mix_start},$((mix_start + 110))p" "$file")
capture_rel=$(echo "${mix_ctx}" | rg -n "native_capture_total_mix_snapshot_from_buffer\\(" | head -n 1 | cut -d: -f1 || true)
fx_rel=$(echo "${mix_ctx}" | rg -n "Apply master FX chain to combined audio" | head -n 1 | cut -d: -f1 || true)
sampler_rel=$(echo "${mix_ctx}" | rg -n "Capture audio for sampler BEFORE master volume scaling" | head -n 1 | cut -d: -f1 || true)

if [ -z "${capture_rel}" ] || [ -z "${fx_rel}" ] || [ -z "${sampler_rel}" ]; then
  echo "FAIL: Could not locate capture/fx/sampler markers in mix path" >&2
  exit 1
fi
if [ "${capture_rel}" -le "${fx_rel}" ]; then
  echo "FAIL: Snapshot capture must happen after master FX in mix path" >&2
  exit 1
fi
if [ "${capture_rel}" -ge "${sampler_rel}" ]; then
  echo "FAIL: Snapshot capture must happen before sampler capture in mix path" >&2
  exit 1
fi
if ! echo "${mix_ctx}" | rg -q "float me_input_scale = \\(mv < 1\\.0f\\) \\? mv : 1\\.0f;"; then
  echo "FAIL: Mix path missing unified ME pre-scale topology" >&2
  exit 1
fi

echo "PASS: Native resample bridge wiring present"
exit 0
