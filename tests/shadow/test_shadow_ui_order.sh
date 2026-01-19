#!/usr/bin/env bash
set -euo pipefail

# Test: Verify post-ioctl MIDI forwarding to shadow UI happens AFTER real_ioctl
# MIDI input arrives during the ioctl, so we must capture after transaction completes
# Note: There's also pre-ioctl forwarding in shadow_filter_move_input, but the
# critical post-ioctl filter is what ensures Move doesn't see shadow UI CCs.

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Find the real_ioctl call
ioctl_line=$(rg -n "real_ioctl\\(fd" "$file" | head -n 1 | cut -d: -f1 || true)

# Find the POST-IOCTL comment which marks the critical section
post_ioctl_comment=$(rg -n "POST-IOCTL.*FILTER.*INCOMING.*MIDI" "$file" | head -n 1 | cut -d: -f1 || true)

if [ -z "${ioctl_line}" ] || [ -z "${post_ioctl_comment}" ]; then
  echo "Failed to locate real_ioctl or POST-IOCTL section in ${file}" >&2
  exit 1
fi

if [ "${post_ioctl_comment}" -gt "${ioctl_line}" ]; then
  echo "PASS: POST-IOCTL MIDI filtering section is after real_ioctl (line ${post_ioctl_comment} > ${ioctl_line})"
  exit 0
fi

echo "FAIL: POST-IOCTL section is before real_ioctl (line ${post_ioctl_comment} <= ${ioctl_line})" >&2
exit 1
