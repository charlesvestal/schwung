#!/usr/bin/env bash
set -euo pipefail

# Runtime helper logs must not write to root /tmp.

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

files=(
  src/shim-entrypoint.sh
  src/restart-move.sh
  src/move_anything_shim.c
  src/host/shadow_process.c
  src/host/web_shim.c
)

pattern='/tmp/(display-server\.log|restart-move\.log|move-shim\.log|link-subscriber\.log|web_shim\.log)'

if rg -n "$pattern" "${files[@]}" >/dev/null 2>&1; then
  echo "FAIL: runtime helpers still route logs to root /tmp" >&2
  rg -n "$pattern" "${files[@]}" >&2
  exit 1
fi

for file in src/host/web_shim.c src/host/display_server.c src/host/link_subscriber.cpp src/host/shadow_process.c; do
  if ! rg -n '(unified_log|LOG_INFO|LOG_WARN|LOG_ERROR|LOG_DEBUG)' "$file" >/dev/null 2>&1; then
    echo "FAIL: $file does not use the unified logging system" >&2
    exit 1
  fi
done

if ! rg -n 'unified-log' src/restart-move.sh >/dev/null 2>&1; then
  echo "FAIL: restart-move.sh does not route shell logs through unified-log" >&2
  exit 1
fi

echo "PASS: runtime helpers do not route logs to root /tmp"
exit 0
