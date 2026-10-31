#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# EVERY REFUSAL MUST REACH THE USER IN WORDS, and that path crosses three
# tables in three languages: the enum (step_plock.h), the token it is printed
# as (shadow_lanes_plock_reason_name, shadow_chain_mgmt.c), and the sentence
# the grid shows (PLOCK_REFUSAL_TEXT, page_controller.mjs).
#
# A code added to the enum and not to the other two does not fail anything --
# it reaches the device as "unknown", or as a raw token, which is exactly the
# defect PLOCK_REFUSAL_TEXT was written to fix ("NOT LOCKED: UNKNOWN PARAM").
# So the tables are pinned to EACH OTHER rather than to a list restated here.

fail() { echo "FAIL: $1"; exit 1; }

enum=$(sed -n '/^enum {/,/^};/p' src/host/step_plock.h |
       grep -oE 'STEP_PLOCK_[A-Z_]+' | grep -v 'STEP_PLOCK_OK' | sort -u)
[ -n "$enum" ] || fail "could not read the STEP_PLOCK_* enum"

names=$(sed -n '/shadow_lanes_plock_reason_name/,/^}/p' src/host/shadow_chain_mgmt.c)
[ -n "$names" ] || fail "could not read shadow_lanes_plock_reason_name"

ui=$(sed -n '/const PLOCK_REFUSAL_TEXT = {/,/};/p' src/shared/param_pages/page_controller.mjs)
[ -n "$ui" ] || fail "could not read PLOCK_REFUSAL_TEXT"

for e in $enum; do
  grep -q "case $e:" <<<"$names" \
    || fail "$e has no token in shadow_lanes_plock_reason_name -- it reaches the device as \"unknown\""
  # The token as printed, e.g. STEP_PLOCK_CLIP_PENDING -> clip_pending. The UI
  # lowercases and replaces underscores with spaces before the lookup.
  tok=$(grep "case $e:" <<<"$names" | sed -E 's/.*return "([a-z_]+)".*/\1/')
  [ -n "$tok" ] || fail "could not read $e's token"
  words=${tok//_/ }
  grep -q "\"$words\"" <<<"$ui" \
    || fail "$e prints \"$tok\" and PLOCK_REFUSAL_TEXT has no \"$words\" -- the user gets the raw token"
done

# "clip pending" is GONE, and must not come back. It printed "new clip, try
# again in a moment" for two states that do not end by waiting -- no clip on
# the track, and the live model not being read -- and a user with NO clip got
# it on every refused lock (field report 2026-10-24). The live model has no
# new-clip blind window, so there is nothing left for "pending" to mean.
# Behaviour is driven in tests/host/test_slot_clip_phase.c.
! grep -q 'CLIP_PENDING' src/host/step_plock.h src/host/shadow_chain_mgmt.c \
  || fail "STEP_PLOCK_CLIP_PENDING is back -- a permanent state told to 'try again'"
! grep -q 'try again in a moment' src/shared/param_pages/page_controller.mjs \
  || fail "a p-lock refusal tells the user to wait again"
body=$(awk '/^static int shadow_lanes_step_phase\(/,/^\}/' src/host/shadow_chain_mgmt.c)
grep -q 'return STEP_PLOCK_NO_CLIP' <<<"$body" \
  || fail "shadow_lanes_step_phase no longer reports a missing clip as NO_CLIP"
grep -q 'return STEP_PLOCK_NO_MODEL' <<<"$body" \
  || fail "shadow_lanes_step_phase no longer separates 'model not live' from 'no clip'"

echo "PASS: every plock refusal crosses all three tables ($(wc -w <<<"$enum") codes)"
