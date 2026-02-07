#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# 1) Native sampler source state should exist (separate from custom sampler_source).
if ! rg -q "native_sampler_source_t" "$file"; then
  echo "FAIL: Missing native sampler source enum/state" >&2
  exit 1
fi

# 2) D-Bus text handler should parse and update native sampler source from stock Move announcements.
if ! rg -q "native_sampler_source_from_text\\(" "$file"; then
  echo "FAIL: Missing native sampler source parser" >&2
  exit 1
fi

if ! rg -q "native_sampler_update_from_dbus_text\\(" "$file"; then
  echo "FAIL: Missing D-Bus native source update hook" >&2
  exit 1
fi

# 3) Post-ioctl path should apply resample bridge into AUDIO_IN when native source is resampling.
if ! rg -q "native_resample_bridge_apply\\(" "$file"; then
  echo "FAIL: Missing native resample bridge apply function" >&2
  exit 1
fi

bridge_fn_start=$(rg -n "static void native_resample_bridge_apply\\(void\\)" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${bridge_fn_start}" ]; then
  echo "FAIL: Could not locate native_resample_bridge_apply() body" >&2
  exit 1
fi
bridge_ctx=$(sed -n "${bridge_fn_start},$((bridge_fn_start + 40))p" "$file")
if ! echo "${bridge_ctx}" | rg -q "native_resample_bridge_source_allows_apply\\("; then
  echo "FAIL: Bridge source-allow helper is not used in apply path" >&2
  exit 1
fi

if ! echo "${bridge_ctx}" | rg -q "native_resample_bridge_mode"; then
  echo "FAIL: Bridge mode state is not used in apply path" >&2
  exit 1
fi

# 3b) Source allow helper should be sticky: allow unknown/line-in/resampling, reject mic/usb-c.
allow_fn_start=$(rg -n "static int native_resample_bridge_source_allows_apply\\(void\\)" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${allow_fn_start}" ]; then
  echo "FAIL: Missing native_resample_bridge_source_allows_apply()" >&2
  exit 1
fi
allow_ctx=$(sed -n "${allow_fn_start},$((allow_fn_start + 60))p" "$file")
if ! echo "${allow_ctx}" | rg -q "NATIVE_SAMPLER_SOURCE_MIC_IN"; then
  echo "FAIL: Source allow helper must explicitly handle MIC_IN" >&2
  exit 1
fi
if ! echo "${allow_ctx}" | rg -q "NATIVE_SAMPLER_SOURCE_USB_C_IN"; then
  echo "FAIL: Source allow helper must explicitly handle USB_C_IN" >&2
  exit 1
fi
if ! echo "${allow_ctx}" | rg -q "NATIVE_SAMPLER_SOURCE_LINE_IN"; then
  echo "FAIL: Source allow helper should explicitly allow LINE_IN" >&2
  exit 1
fi
if ! echo "${allow_ctx}" | rg -q "NATIVE_SAMPLER_SOURCE_UNKNOWN"; then
  echo "FAIL: Source allow helper should include UNKNOWN fallback" >&2
  exit 1
fi

# 3c) Capture should happen pre-volume in mix path (not at ioctl sync point).
mix_fn_start=$(rg -n "static void shadow_inprocess_mix_from_buffer\\(void\\)" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${mix_fn_start}" ]; then
  echo "FAIL: Could not locate shadow_inprocess_mix_from_buffer()" >&2
  exit 1
fi
mix_ctx=$(sed -n "${mix_fn_start},$((mix_fn_start + 90))p" "$file")
capture_line_rel=$(echo "${mix_ctx}" | rg -n "native_capture_total_mix_snapshot_from_buffer\\(" | head -n 1 | cut -d: -f1 || true)
volume_line_rel=$(echo "${mix_ctx}" | rg -n "Apply master volume|float mv = shadow_master_volume" | head -n 1 | cut -d: -f1 || true)
if [ -z "${capture_line_rel}" ] || [ -z "${volume_line_rel}" ]; then
  echo "FAIL: Could not locate capture call and master volume section in mix path" >&2
  exit 1
fi
if [ "${capture_line_rel}" -ge "${volume_line_rel}" ]; then
  echo "FAIL: Native snapshot capture must happen before master volume scaling" >&2
  exit 1
fi

# Ensure old pre-ioctl snapshot call is gone.
if rg -q "native_capture_total_mix_snapshot\\(\\);" "$file"; then
  echo "FAIL: Old pre-ioctl snapshot call is still present; capture must happen in pre-volume mix path" >&2
  exit 1
fi

# 4) Mailbox probe path should not be present in this mode (D-Bus-only detection).
if rg -q "native_source_probe_mailbox\\(" "$file"; then
  echo "FAIL: Mailbox source probe path is still present" >&2
  exit 1
fi

echo "PASS: Native resample bridge wiring present"
exit 0
