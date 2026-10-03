#!/usr/bin/env bash
# schwung-testd must claim the param channel the way shadow_ui and
# schwung-manager do, or it destroys their answers.
#
# It used to wait for request_type == 0 and then zero response_ready while
# filling in its own request. request_type clears in the same breath as an
# answer is published, so that erased the other client's answer unread --
# the defect shadow_ui.c's shadow_param_claim documents fixing on its own
# side. On a Move it made a set switch under test lose nearly every restore
# answer (~70 s per switch, and with the old restore code a corrupted set).
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
F="$ROOT/src/host/test_daemon/commands.c"
fail=0
no() { if grep -qE "$1" "$F"; then echo "FAIL: $2"; fail=1; fi; }
yes() { if ! grep -qE "$1" "$F"; then echo "FAIL: $2"; fail=1; fi; }
no  'request_type, __ATOMIC_ACQUIRE\) != 0' "testd waits on request_type alone again (the answer-destroying claim)"
yes '__atomic_compare_exchange_n\(testd_param_head\(\)' "testd must claim with a compare-exchange on the head word"
yes 'TESTD_PARAM_RR_MASK\) != 0' "testd's claim must refuse a channel holding an unread answer"
# every answer read must be followed by a consume
reads=$(grep -c 'testd_param_wait_response(req_id' "$F")
consumes=$(grep -c 'testd_param_consume();' "$F")
if [ "$consumes" -lt "$reads" ]; then
  echo "FAIL: $reads requests but only $consumes consumes -- an answer left unread wedges the channel for 250 ms"; fail=1
fi
# nothing reads the shared value after the channel is handed back
if awk '/testd_param_consume\(\);/{c=1} /^}/{c=0} c && /g_shm\.param->value/' "$F" | grep -q .; then
  echo "FAIL: g_shm.param->value is read after testd_param_consume() -- another client may own it by then"; fail=1
fi
[ "$fail" = 0 ] && echo "PASS: testd claims and consumes like the other param clients"
exit "$fail"
