#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

hdr=src/modules/chain/dsp/chain_internal.h

# Read the cap out of the shipped header rather than restating it, so the
# per-bus cases track the range buses actually run with. bus_route.h takes
# bus_count as a parameter and holds no copy of the cap; this proves full
# coverage of whatever the shipped value is, plus rejection of the first past it.
cap=$(awk '/^#define SLOT_BUSES /{print $3}' "$hdr")
if [ -z "$cap" ]; then
  echo "FAIL: could not read SLOT_BUSES from $hdr" >&2
  exit 1
fi

bin="build/tests/test_bus_route"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host \
  -DTEST_SLOT_BUSES="$cap" \
  tests/host/test_bus_route.c \
  -o "$bin"

"$bin"
