#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

hdr=src/host/shadow_chain_mgmt.h

# Read the cap out of the shipped header rather than restating it, so the
# per-slot cases track the range Master FX actually runs with. The routing in
# master_fx_key.h takes slot_count as a parameter and holds no copy of the cap;
# this only proves full coverage of whatever the shipped value is, plus
# rejection of the first slot past it.
cap=$(awk '/^#define MASTER_FX_SLOTS /{print $3}' "$hdr")
if [ -z "$cap" ]; then
  echo "FAIL: could not read MASTER_FX_SLOTS from $hdr" >&2
  exit 1
fi

bin="build/tests/test_master_fx_slot_routing"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host \
  -DTEST_MASTER_FX_SLOTS="$cap" \
  tests/host/test_master_fx_slot_routing.c \
  -o "$bin"

"$bin"
