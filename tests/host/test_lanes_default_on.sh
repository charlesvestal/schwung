#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
# AUTOMATION LANES ARE ON BY DEFAULT; /data/UserData/schwung/lanes_off is the
# kill switch. It was an opt-in (`lanes_on`), and a reinstall that did not
# carry the file turned the feature off with nothing on screen but
# "NOT LOCKED: DISABLED". Pinned: the worker polls lanes_off (not lanes_on),
# and the shim pushes enabled = NOT that flag.
fail=0
grep -q '"/data/UserData/schwung/lanes_off",[[:space:]]*SHIM_FLAG_LANES_OFF' src/host/shim_worker.c \
  || { echo "FAIL: shim_worker does not poll lanes_off as the kill switch"; fail=1; }
if grep -q 'schwung/lanes_on"' src/host/shim_worker.c; then
  echo "FAIL: lanes_on is polled again -- lanes must not be opt-in"; fail=1
fi
grep -q 'const int en = (shim_debug_flags & SHIM_FLAG_LANES_OFF) ? 0 : 1;' src/schwung_shim.c \
  || { echo "FAIL: the shim does not push lanes:enabled as NOT lanes_off"; fail=1; }
[ $fail = 0 ] && echo "PASS: automation lanes are on unless lanes_off exists"
exit $fail
