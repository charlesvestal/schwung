#!/usr/bin/env bash
# The slot mute/solo mutators run on the SPI CALLBACK: Move's model edges are
# applied there (move_model_sync_apply_pending), and so are the Mute+Track
# combo and the slot:muted param serve. shadow_log() is unified_log(), which
# with debug_log_on armed is fopen/fprintf/fflush -- file I/O on the callback,
# on every mute or solo Move makes. They used to call it; the comments around
# them said shadow_log() was a no-op there, and it is not.
#
# So: none of these bodies may log or touch a file, and the worker must be the
# one that reports the change (shadow_mix_log_service).
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/host/shadow_chain_mgmt.c
WORKER=src/host/shim_worker.c
fails=0
for fn in shadow_apply_mute shadow_apply_solo shadow_apply_mix_state shadow_apply_volume; do
  body=$(awk -v f="$fn" '$0 ~ "^void "f"\\(" {on=1} on {print} on && /^}/ {exit}' "$SRC")
  if [ -z "$body" ]; then echo "FAIL $fn not found in $SRC"; fails=$((fails+1)); continue; fi
  if echo "$body" | grep -qE '\b(shadow_log|unified_log|LOG_DEBUG|LOG_INFO|LOG_WARN|LOG_ERROR|fopen|fprintf|fwrite|fflush|printf)\s*\('; then
    echo "FAIL $fn logs or does file I/O -- it runs on the SPI callback"
    echo "$body" | grep -nE '\b(shadow_log|unified_log|LOG_[A-Z]+|fopen|fprintf|fwrite|fflush|printf)\s*\(' | sed 's/^/     /'
    fails=$((fails+1))
  else
    echo "ok   $fn does not log"
  fi
done
if grep -qE '^\s*shadow_mix_log_service\(\);' "$WORKER"; then
  echo "ok   the worker logs mix changes"
else
  echo "FAIL $WORKER no longer calls shadow_mix_log_service(); mute/solo changes go unlogged"
  fails=$((fails+1))
fi
[ "$fails" -eq 0 ]
