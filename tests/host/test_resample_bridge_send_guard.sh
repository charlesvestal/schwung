#!/usr/bin/env bash
#
# The native resample bridge may only reconstruct a mix from
# native_bridge_me_component while nothing has been added to the ME bus since
# that buffer was snapshotted. Master FX and the send returns are both such
# additions, and the guard originally named only Master FX — so a send with a
# return level up produced a resample that differed from the DAC and from the
# sampler/skipback captures, silently.
#
# The behaviour of the predicate itself is covered by test_me_post_snapshot_fx.
# What a compiled test cannot see is the CALL SITE choosing the narrow
# predicate again, which is exactly the regression, so it is pinned here.
set -euo pipefail

file="src/host/shadow_resample.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -q "shadow_me_post_snapshot_fx_active\(\)" "$file"; then
  echo "FAIL: $file must gate the split reconstruction on shadow_me_post_snapshot_fx_active()" >&2
  exit 1
fi

# Nothing in this file may ask about Master FX alone. Both the guard and the
# diagnostic line report "is the split usable", and Master FX is only half of
# that question.
if rg -q "shadow_master_fx_chain_active\(\)" "$file"; then
  echo "FAIL: $file still asks shadow_master_fx_chain_active() — that ignores an active send bus" >&2
  rg -n "shadow_master_fx_chain_active\(\)" "$file" >&2
  exit 1
fi

# The guard is the conjunction: post-snapshot FX absent AND a snapshot taken.
if ! rg -q "!shadow_me_post_snapshot_fx_active\(\) && native_bridge_split_valid" "$file"; then
  echo "FAIL: split reconstruction guard is not '!post_snapshot_fx && split_valid'" >&2
  exit 1
fi

echo "PASS: resample bridge split guard covers the send buses"
exit 0
