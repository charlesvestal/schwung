#!/usr/bin/env bash
# The surface claims its OWN MESSAGES, not the whole cable.
#
# The first implementation consumed every cable-2 event while the setting was
# on. That is too broad: it silences the CC Map for any other device sharing
# cable 2 for as long as the surface is switched on -- one feature's setting
# breaking a different feature, with nothing on screen to explain it.
#
# Narrowing is also what makes the chain-side check redundant. A claimed
# message never reaches the chain because the shim consumes it first, which is
# the enforcement point the plan preferred: the shim already reads the flag,
# and ownership is decided where the message is routed.
set -euo pipefail
cd "$(dirname "$0")/../.."

out="${TMPDIR:-/tmp}/schwung_e16_claim_msg"
cc -I src/host tests/host/test_e16_claim_msg.c -o "$out"
"$out"

# The shim must actually consult it. Without this the header is correct and
# unused -- which is precisely the shape of the gap this plan hit three times.
#
# Matched on the CALL, not on the argument spelling. This pin named
# `(1, status, d1)` verbatim, so moving the claim out of the display-gated
# block -- where its locals are called `st` and `e_d1` -- failed it, reporting
# "the shim does not consult e16_claims_msg" about a shim that consults it on
# the very next line. A pin that fails on a rename defends a spelling rather
# than a fact, and the noise teaches you to edit the test.
grep -qE "e16_claims_msg\([^)]*\)" src/schwung_shim.c \
  || { echo "FAIL: the shim does not consult e16_claims_msg"; exit 1; }
echo "PASS: the shim claims only the surface's own messages"
