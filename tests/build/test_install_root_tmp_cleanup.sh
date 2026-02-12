#!/usr/bin/env bash
set -euo pipefail

# Ensure install preflight cleans stale root tmp artifacts before root-space guard.

file="scripts/install.sh"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

cleanup_line=$(rg -n "/var/volatile/tmp/_MEI\\*" "$file" | head -n 1 | cut -d: -f1 || true)
guard_line=$(rg -n "root_avail=.*df /" "$file" | head -n 1 | cut -d: -f1 || true)

if [ -z "${cleanup_line}" ]; then
  echo "FAIL: install.sh missing stale tmp cleanup command for /var/volatile/tmp/_MEI*" >&2
  exit 1
fi

if [ -z "${guard_line}" ]; then
  echo "FAIL: install.sh missing root_avail guard" >&2
  exit 1
fi

if [ "${cleanup_line}" -ge "${guard_line}" ]; then
  echo "FAIL: tmp cleanup must run before root_avail guard (${cleanup_line} >= ${guard_line})" >&2
  exit 1
fi

echo "PASS: install.sh cleans stale root tmp artifacts before root-space check"
exit 0
