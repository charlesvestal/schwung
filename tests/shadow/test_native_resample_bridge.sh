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
if ! echo "${bridge_ctx}" | rg -q "native_sampler_source != NATIVE_SAMPLER_SOURCE_RESAMPLING"; then
  echo "FAIL: Bridge source gating check missing" >&2
  exit 1
fi

if ! echo "${bridge_ctx}" | rg -q "native_resample_bridge_mode"; then
  echo "FAIL: Bridge mode state is not used in apply path" >&2
  exit 1
fi

bridge_line=$(rg -n "native_resample_bridge_apply\\(" "$file" | tail -n 1 | cut -d: -f1 || true)
copy_line=$(rg -n "memcpy\\(shadow_mailbox \\+ AUDIO_IN_OFFSET, hardware_mmap_addr \\+ AUDIO_IN_OFFSET" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${bridge_line}" ] || [ -z "${copy_line}" ]; then
  echo "FAIL: Could not locate bridge apply and AUDIO_IN copy lines" >&2
  exit 1
fi

if [ "${bridge_line}" -le "${copy_line}" ]; then
  echo "FAIL: Bridge apply must run after hardware AUDIO_IN copy (line ${bridge_line} <= ${copy_line})" >&2
  exit 1
fi

# 4) Mailbox probe path should not be present in this mode (D-Bus-only detection).
if rg -q "native_source_probe_mailbox\\(" "$file"; then
  echo "FAIL: Mailbox source probe path is still present" >&2
  exit 1
fi

echo "PASS: Native resample bridge wiring present"
exit 0
