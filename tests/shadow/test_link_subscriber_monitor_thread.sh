#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -q "link_sub_monitor_main\\(" "$file"; then
  echo "FAIL: Missing link_sub_monitor_main() thread entrypoint" >&2
  exit 1
fi

if ! rg -q "pthread_create\\(&link_sub_monitor_thread" "$file"; then
  echo "FAIL: Missing pthread_create for link_sub monitor thread" >&2
  exit 1
fi

ioctl_start=$(rg -n "^int ioctl\\(int fd, unsigned long request, \\.\\.\\.\\)" "$file" | head -n 1 | cut -d: -f1 || true)
ioctl_guard=$(rg -n "if \\(baseline_mode\\) goto do_ioctl;" "$file" | head -n 1 | cut -d: -f1 || true)

if [ -z "${ioctl_start}" ] || [ -z "${ioctl_guard}" ]; then
  echo "FAIL: Could not locate ioctl() block in ${file}" >&2
  exit 1
fi

ioctl_block=$(sed -n "${ioctl_start},${ioctl_guard}p" "$file")

if echo "${ioctl_block}" | rg -q "Link audio stale detected|link_sub_kill_pending|link_sub_wait_countdown|link_sub_restart_cooldown"; then
  echo "FAIL: Link subscriber stale/restart logic is still in ioctl hot path" >&2
  exit 1
fi

echo "PASS: Link subscriber stale/restart logic runs outside ioctl hot path"
exit 0
