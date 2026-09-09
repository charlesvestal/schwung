#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The ARITHMETIC is run by tests/host/test_e16_cc_claim.c against
# e16_claim.h. This is the other half: chain_midi.c's CC-map lookup
# (v2_on_midi, "Handle knob CC mappings") cannot be compiled natively — it
# dlopens plugins and owns the get_param/set_param surface — so what the C
# test cannot see is pinned at the source level here:
#
#   1. the CC-map lookup calls e16_claims_cc() rather than restating the
#      claimed range inline, and
#   2. the claimed range is defined exactly once (in e16_claim.h), never
#      duplicated as a second pair of literals anywhere in the tree.

fail() { echo "FAIL: $1"; exit 1; }

midi="src/modules/chain/dsp/chain_midi.c"
hdr="src/host/e16_claim.h"

[ -f "$hdr" ] || fail "$hdr is missing"

# ------------------------------------------------------- 1. the call site
grep -q 'e16_claim\.h"' "$midi" \
  || fail "chain_midi.c does not include e16_claim.h"
grep -q 'e16_claims_cc(' "$midi" \
  || fail "the CC-map lookup no longer calls e16_claims_cc()"

# ------------------------------------------------------- 2. one definition
# The claimed range (1-16, channel 1 / wire value 0) must be defined only in
# e16_claim.h. A second copy of "1" and "16" beside a CC/channel check
# anywhere else in the tree is exactly how cc_reserved-class bugs happen: two
# owners of one fact, and one of them drifts.
hits="$(grep -rl 'E16_CLAIMED_CC_LOW\|E16_CLAIMED_CC_HIGH\|E16_CLAIMED_CHANNEL' \
  --include='*.c' --include='*.h' src/ 2>/dev/null || true)"
count="$(echo "$hits" | grep -c . || true)"
for f in $hits; do
  case "$f" in
    "$hdr") ;;
    *) fail "$f restates the E16 claimed-range constants instead of reading $hdr" ;;
  esac
done

# ------------------------------------------------------- 3. the C unit test
bin="build/tests/test_e16_cc_claim"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Isrc/host \
  tests/host/test_e16_cc_claim.c \
  -o "$bin"

"$bin"
