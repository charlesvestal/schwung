#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

hdr=src/host/shadow_chain_mgmt.h
bus=src/host/bus_mix.h

# Read both caps out of the shipped headers rather than restating them, so the
# cases track the range the sends actually run with. send_fx_key.h takes both
# counts as parameters and holds no copy of either; this proves full coverage of
# whatever the shipped values are, plus rejection of the first index past each.
#
# SEND_BUSES and SEND_FX_SLOTS are DEFINED FROM other names (BUS_MIX_SENDS and
# MASTER_FX_SLOTS), which is the point — one number, one definition — so the
# awk has to resolve one hop rather than assume a literal. If either is ever
# changed to something this cannot resolve, the test fails loudly here instead
# of silently testing the wrong range.
resolve() {
  local raw="$1"
  case "$raw" in
    BUS_MIX_SENDS)   awk '/^#define BUS_MIX_SENDS /{print $3}'   "$bus" ;;
    MASTER_FX_SLOTS) awk '/^#define MASTER_FX_SLOTS /{print $3}' "$hdr" ;;
    *)               echo "$raw" ;;
  esac
}

buses=$(resolve "$(awk '/^#define SEND_BUSES /{print $3}' "$hdr")")
slots=$(resolve "$(awk '/^#define SEND_FX_SLOTS /{print $3}' "$hdr")")

for v in buses slots; do
  case "${!v}" in
    ''|*[!0-9]*)
      echo "FAIL: could not resolve $v to a number from $hdr / $bus (got '${!v}')" >&2
      exit 1 ;;
  esac
done

bin="build/tests/test_send_fx_key"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host \
  -DTEST_SEND_BUSES="$buses" \
  -DTEST_SEND_FX_SLOTS="$slots" \
  tests/host/test_send_fx_key.c \
  -o "$bin"

"$bin"
