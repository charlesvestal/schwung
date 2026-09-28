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

# And the pending case specifically, because it is the one that was WRONG
# rather than missing: a brand-new clip reported "no clip on this track" while
# the user was looking at one. Measured 2026-09-14: the slot is unknown for
# 8-12 s, ending the second Move writes Song.abl.
grep -q 'STEP_PLOCK_CLIP_PENDING' src/host/step_plock.h \
  || fail "the pending-clip reason is gone -- a new clip refuses as though it were absent"
grep -q 'cslot < 0 && !(clip_len > 0.0)) return STEP_PLOCK_CLIP_PENDING' src/host/shadow_chain_mgmt.c \
  || fail "pending is no longer gated on having no row AND no length -- either it claims a clip on an EMPTY track, or it refuses a gesture that LANE_SLOT_PENDING can now land"
grep -q 'LANE_SLOT_PENDING' src/host/shadow_chain_mgmt.c \
  || fail "the host no longer reports a pending row -- the blind window refuses again"

echo "PASS: every plock refusal crosses all three tables ($(wc -w <<<"$enum") codes)"
