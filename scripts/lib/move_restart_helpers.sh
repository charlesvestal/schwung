#!/usr/bin/env bash

wait_for_move_shim_mapping() {
  local attempts="${1:-15}"
  local i

  for i in $(seq 1 "$attempts"); do
    ssh_ableton_with_retry "sleep 1" || true
    if $ssh_root "pid=\$(pidof MoveOriginal 2>/dev/null | awk '{print \$1}'); test -n \"\$pid\" && tr '\\0' '\\n' < /proc/\$pid/environ | grep -q 'LD_PRELOAD=move-anything-shim.so' && grep -q 'move-anything-shim.so' /proc/\$pid/maps" 2>/dev/null; then
      return 0
    fi
  done

  return 1
}

direct_start_move_with_shim() {
  qecho "Init service did not relaunch Move; trying direct launch fallback..."

  ssh_root_with_retry "for name in MoveOriginal Move MoveLauncher MoveMessageDisplay shadow_ui move-anything link-subscriber display-server; do pids=\$(pidof \$name 2>/dev/null || true); if [ -n \"\$pids\" ]; then kill -9 \$pids 2>/dev/null || true; fi; done" || true
  ssh_root_with_retry "rm -f /dev/shm/move-shadow-* /dev/shm/move-display-*" || true
  ssh_root_with_retry "pids=\$(fuser /dev/ablspi0.0 2>/dev/null || true); if [ -n \"\$pids\" ]; then kill -9 \$pids || true; fi" || true
  ssh_root_with_retry "su -s /bin/sh ableton -c 'nohup /opt/move/Move >/tmp/move-shim.log 2>&1 &'" || return 1

  return 0
}

restart_move_with_fallback() {
  local fail_msg="$1"
  local init_attempts="${2:-15}"
  local fallback_attempts="${3:-30}"

  ssh_root_with_retry "/etc/init.d/move start >/dev/null 2>&1" || fail "Failed to restart Move service"

  if wait_for_move_shim_mapping "$init_attempts"; then
    return 0
  fi

  direct_start_move_with_shim || fail "$fail_msg"
  wait_for_move_shim_mapping "$fallback_attempts" || fail "$fail_msg"
}
