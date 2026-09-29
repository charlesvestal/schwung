#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A SET'S AUTOMATION MUST NOT DISAPPEAR ACROSS A RESTART.
#
# Observed twice on the device: three lanes and 51 points gone after a deploy,
# recoverable only because a copy had been taken by hand. The path is not a
# corrupt write -- it is two correct-looking steps that combine:
#
#   restoreSlotLanes() pushes the file into the DSP with setSlotParam and does
#   not check that it landed, while setting the write cache as though it had.
#   The param channel is busiest exactly there -- boot, behind a chain that is
#   still instantiating -- so the push can fail silently.
#
#   persistSlotLanes() then asks the slot, gets "" (served-and-empty, a
#   perfectly good answer for a DSP with no lanes), sees the cache disagree,
#   and DELETES the file.
#
# Neither half is wrong on its own, which is why this needs a test that pins
# the JOIN: a slot may only clear its file once a restore has been CONFIRMED.

fail() { echo "FAIL: $1"; exit 1; }
ui=src/shadow/shadow_ui.js

grep -q 'let laneRestoreConfirmed' "$ui" \
  || fail "laneRestoreConfirmed is gone -- the autosave can clear a file whose restore never landed"

restore=$(awk '/^function restoreSlotLanes/,/^}/' "$ui")
[ -n "$restore" ] || fail "restoreSlotLanes not found"
echo "$restore" | grep -q 'getSlotStateWithRetry(i, "lanes:state")' \
  || fail "the restore must READ BACK what it pushed -- an unchecked write is how the document is lost"
echo "$restore" | grep -q 'laneRestoreConfirmed\[i\] = true' \
  || fail "the restore must confirm the slot on success"
# ...and the two early returns are confirmations too: an ABSENT or EMPTY file
# is positive knowledge that the slot owns nothing, which is exactly when
# clearing is correct.
n=$(echo "$restore" | grep -c 'laneRestoreConfirmed\[i\] = true')
[ "$n" = "3" ] || fail "expected three confirmations (absent file, empty file, readback) -- found $n"

persist=$(awk '/^function persistSlotLanes/,/^}/' "$ui")
[ -n "$persist" ] || fail "persistSlotLanes not found"
echo "$persist" | grep -q 'if (!laneRestoreConfirmed\[i\]) return;' \
  || fail "the DELETE branch must refuse an unconfirmed slot, or a failed restore erases the user's automation"

# The guard has to sit INSIDE the empty-document branch: hoisting it would stop
# an unconfirmed slot from WRITING too, which loses a take recorded before the
# first successful restore.
python3 - <<'PY' || fail "the confirmation guard is not inside the doc === \"\" branch"
import sys
src = open("src/shadow/shadow_ui.js").read()
i = src.index("function persistSlotLanes")
body = src[i:src.index("\n}", i)]
empty = body.index('if (doc === "") {')
guard = body.index("if (!laneRestoreConfirmed[i]) return;")
nul = body.index('if (doc === null) return;')
sys.exit(0 if nul < empty < guard else 1)
PY

echo "PASS: a slot cannot clear its lane file until a restore is confirmed"
