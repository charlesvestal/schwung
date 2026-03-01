#!/usr/bin/env bash
set -euo pipefail

HELPER="scripts/lib/move_restart_helpers.sh"
SCRIPT="scripts/install.sh"

if [ ! -f "$HELPER" ]; then
  echo "FAIL: restart helper missing at $HELPER" >&2
  exit 1
fi

# shellcheck source=/dev/null
source "$HELPER"

scenario=""
fallback_started=0
declare -a commands=()

fail() {
  echo "FAIL: $*" >&2
  return 1
}

qecho() {
  :
}

ssh_ableton_with_retry() {
  return 0
}

ssh_root=ssh_root

ssh_root() {
  local cmd="$1"
  case "$cmd" in
    *"grep -q 'LD_PRELOAD=move-anything-shim.so'"*"grep -q 'move-anything-shim.so' /proc/"*)
      case "$scenario" in
        service-success)
          return 0
          ;;
        fallback-success)
          [ "$fallback_started" -eq 1 ]
          return
          ;;
      esac
      return 1
      ;;
  esac
  return 0
}

ssh_root_with_retry() {
  local cmd="$1"
  commands+=("$cmd")
  case "$cmd" in
    *"nohup /opt/move/Move >/tmp/move-shim.log 2>&1 &"*)
      fallback_started=1
      ;;
  esac
  return 0
}

assert_contains() {
  local needle="$1"
  local cmd
  for cmd in "${commands[@]}"; do
    if [[ "$cmd" == *"$needle"* ]]; then
      return 0
    fi
  done
  echo "FAIL: expected command containing: $needle" >&2
  exit 1
}

assert_not_contains() {
  local needle="$1"
  local cmd
  for cmd in "${commands[@]}"; do
    if [[ "$cmd" == *"$needle"* ]]; then
      echo "FAIL: unexpected command containing: $needle" >&2
      exit 1
    fi
  done
}

run_service_success_case() {
  scenario="service-success"
  fallback_started=0
  commands=()

  restart_move_with_fallback "service path should succeed" 2 2

  assert_contains "/etc/init.d/move start >/dev/null 2>&1"
  assert_not_contains "nohup /opt/move/Move >/tmp/move-shim.log 2>&1 &"
}

run_fallback_case() {
  scenario="fallback-success"
  fallback_started=0
  commands=()

  restart_move_with_fallback "fallback path should succeed" 2 2

  assert_contains "/etc/init.d/move start >/dev/null 2>&1"
  assert_contains "nohup /opt/move/Move >/tmp/move-shim.log 2>&1 &"
}

run_service_success_case
run_fallback_case

if ! grep -q 'restart_move_with_fallback "Move started without active shim (LD_PRELOAD check failed)"' "$SCRIPT"; then
  echo "FAIL: install.sh re-enable path is not using restart_move_with_fallback" >&2
  exit 1
fi

if ! grep -q 'restart_move_with_fallback "Move started without active shim mapping (LD_PRELOAD env/maps check failed)"' "$SCRIPT"; then
  echo "FAIL: install.sh main install path is not using restart_move_with_fallback" >&2
  exit 1
fi

echo "PASS: install restart helper uses direct launch fallback when init service leaves MoveLauncher idle"
