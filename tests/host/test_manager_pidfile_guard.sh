#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The schwung-manager start guard in src/schwung-entry.sh.
#
# The bug this pins, observed on hardware 2026-08-20: the guard was
#
#     if [ -f "$PID" ] && kill -0 "$(cat "$PID")" 2>/dev/null; then : ; fi
#
# `kill -0` asks whether SOMETHING holds that pid, not whether the MANAGER
# does. The pid file survives a reboot and Linux reissues the number, so after
# an install the stale pid 928 came back as `display-server`. The guard passed,
# the manager was never started, and port 7700 was dead with no error anywhere
# — the manager log simply stopped, which reads like a clean shutdown.
#
# It only misfires after a reboot, and only when that one number is reused, so
# it is both rare and invisible. Hence a test.
#
# The guard block is LIFTED from the real script rather than restated here, so
# this cannot pass against a script that no longer contains it.

SRC="src/schwung-entry.sh"
[ -f "$SRC" ] || { echo "FAIL: $SRC not found"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Lift the guard: from the SCHWUNG_MGR_RUNNING init to the branch it decides.
python3 - "$SRC" "$TMP/guard.sh" <<'PY'
import sys, re
src, out = sys.argv[1], sys.argv[2]
s = open(src, encoding="utf8").read()
start = s.find("    SCHWUNG_MGR_RUNNING=0")
if start < 0:
    sys.exit("FAIL: the guard no longer initialises SCHWUNG_MGR_RUNNING - "
             "if it was rewritten, update this test deliberately")
end = s.find('if [ "$SCHWUNG_MGR_RUNNING" = "1" ]', start)
if end < 0:
    sys.exit("FAIL: could not find the decision branch after the guard")
open(out, "w", encoding="utf8").write(s[start:end])
PY

fails=0
check() {  # check <label> <expected 0|1>
  local label="$1" expect="$2" got
  got="$(
    SCHWUNG_PROC_DIR="$TMP/proc" SCHWUNG_MGR_PID="$TMP/pid" \
    bash -c 'source "$1"; echo "$SCHWUNG_MGR_RUNNING"' _ "$TMP/guard.sh"
  )"
  if [ "$got" != "$expect" ]; then
    echo "FAIL: $label - expected SCHWUNG_MGR_RUNNING=$expect, got $got"
    fails=$((fails + 1))
  fi
}

# A live process to point the pid file at. `sleep` is not the manager.
sleep 30 &
LIVE=$!
trap 'kill $LIVE 2>/dev/null || true; rm -rf "$TMP"' EXIT
mkdir -p "$TMP/proc/$LIVE"

# 1. THE BUG: pid is live but belongs to something else. Must start the manager.
printf '%s' "$LIVE" > "$TMP/pid"
printf 'display-server\0' > "$TMP/proc/$LIVE/cmdline"
check "a live pid owned by display-server must NOT count as running" 0

# 2. The pid really is the manager. Must skip.
printf './schwung-manager\0-port\0007700\0' > "$TMP/proc/$LIVE/cmdline"
check "a live pid whose cmdline is schwung-manager counts as running" 1

# 3. Dead pid, with a cmdline fixture that would otherwise match.
#    Guards against a rewrite that trusts the fixture and drops the liveness test.
DEAD=999999
mkdir -p "$TMP/proc/$DEAD"
printf './schwung-manager\0' > "$TMP/proc/$DEAD/cmdline"
printf '%s' "$DEAD" > "$TMP/pid"
check "a dead pid must NOT count as running" 0

# 4/5. Garbage and empty pid files must not become /proc//cmdline.
printf 'not-a-pid' > "$TMP/pid"
check "a non-numeric pid file must NOT count as running" 0
: > "$TMP/pid"
check "an empty pid file must NOT count as running" 0

# 6. No pid file at all.
rm -f "$TMP/pid"
check "a missing pid file must NOT count as running" 0

if [ "$fails" -ne 0 ]; then exit 1; fi
echo "PASS: manager pid guard - verifies the pid IS the manager, not merely alive"
