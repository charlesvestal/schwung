#!/usr/bin/env bash
# A SET CHANGE THAT CANNOT BE NAMED MUST NOT BE CONSUMED.
#
# shadow_ui.js reads the incoming set's uuid over /schwung-param, which has ONE
# request slot and can simply be STARVED — it answers empty, which is NOT the
# same fact as "there is no set". The handler used to treat it as the second:
# active_set.txt is only written `if (uuid)` so it kept naming the OUTGOING
# set, `newDir` fell back to the DEFAULT directory, and the flag was cleared at
# the end regardless — so the switch was never retried.
#
# Observed on the device: active_set.txt naming a set the user had DELETED while
# Move played another; set_state/ holding a directory for the dead one and none
# for the live one; the user's p-locks written into the dead set's lane file at
# a row only that set had, never playing. After a restart there was no
# directory to restore from, so no slot came up active and the instruments were
# gone.
#
# THIS IS A SOURCE PIN, and this repo's own notes record that a grep pin is a
# weak instrument — it cannot execute the handler. So it checks the three
# things that actually carry the fix rather than that some text exists: the
# guard exists, it abandons only THIS work (a bare `return` would skip ~900
# lines of tick(), including reconcilePadBlock(), leaving the pads dead), and
# it sits BEFORE the flag is cleared.
set -euo pipefail
cd "$(dirname "$0")/../.."
f=src/shadow/shadow_ui.js
fail() { echo "FAIL: $1" >&2; exit 1; }

grep -q 'if (flags & SHADOW_UI_FLAG_SET_CHANGED) setChange: {' "$f" \
  || fail "the SET_CHANGED handler is no longer a labelled block, so the guard \
below cannot abandon just this work"

# The guard must exist and must not consume the flag.
guard_line=$(grep -n 'if (!uuid) {' "$f" | head -n 1 | cut -d: -f1) \
  || fail "no 'if (!uuid)' guard: an unnamed set change is being acted on"
[ -n "$guard_line" ] || fail "no 'if (!uuid)' guard"

# It must break out of the labelled block, NEVER return from tick().
sed -n "${guard_line},$((guard_line + 14))p" "$f" | grep -q 'break setChange;' \
  || fail "the unnamed-set guard does not 'break setChange' — if it returns \
from tick() it skips reconcilePadBlock() and the pads go dead"
sed -n "${guard_line},$((guard_line + 14))p" "$f" | grep -q '^\s*return;' \
  && fail "the unnamed-set guard RETURNS from tick(); that skips ~900 lines \
including reconcilePadBlock()"

# And it must come before the flag is cleared, or it is consumed anyway.
clear_line=$(grep -n 'shadow_clear_ui_flags(SHADOW_UI_FLAG_SET_CHANGED)' "$f" \
  | head -n 1 | cut -d: -f1)
[ -n "$clear_line" ] || fail "cannot find where SET_CHANGED is cleared"
[ "$guard_line" -lt "$clear_line" ] \
  || fail "the guard (line $guard_line) is AFTER the flag clear (line \
$clear_line): the change is consumed before it is known, so it is never retried"

# The retry has to be bounded: a flag that can never be consumed is its own
# hang, and each retry re-saves the outgoing set.
grep -q 'SET_CHANGE_ID_TRIES' "$f" \
  || fail "the retry is unbounded — a set change that can never be identified \
would re-save the outgoing set on every tick, forever"

echo "PASS: an unnamed set change is retried, not consumed, and only this work is skipped"
