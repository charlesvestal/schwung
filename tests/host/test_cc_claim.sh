#!/usr/bin/env bash
# The generic CC map's claim table (src/host/cc_claim.h), compiled and run; and
# the shim's cable-2 walk consulting it only AFTER the surface's own claim, and
# swallowing through midi_in_swallow (both buffers) -- see the design doc.
set -euo pipefail
cd "$(dirname "$0")/../.."
fail() { echo "FAIL: $1"; exit 1; }
bin="build/tests/test_cc_claim"
mkdir -p "$(dirname "$bin")"
cc -std=gnu11 -Wall -Wextra -Isrc/host tests/host/test_cc_claim.c -o "$bin"
"$bin"

shim=src/schwung_shim.c
grep -q '#include "host/cc_claim.h"' "$shim" || fail "the shim does not include cc_claim.h"
# Order: the surface claim (e16_claims_msg) comes BEFORE the CC map's route, in
# the same walk -- a surface's own encoders must never reach the CC map.
perl -0ne 'exit(!/e16_claims_msg\(1, st, e_d1\).*?cc_claim_route\(/s)' "$shim" \
  || fail "the CC map is consulted before (or without) the surface claim"
perl -0ne 'exit(!/cc_claim_route\([\s\S]{0,900}?midi_in_swallow\(sh_midi, hw_midi, j\)/)' "$shim" \
  || fail "a bound CC is not swallowed from both buffers"
# The shadow UI half (shadow_ui.js cannot be imported under node):
ui=src/shadow/shadow_ui.js
grep -q 'ccMap.feed(data\[0\], data\[1\], data\[2\], claimed)' "$ui" \
  || fail "external CCs do not reach the CC map with the surface claim applied"
grep -q 'host_cc_claim_set(ccMap.claimPairs())' "$ui" \
  || fail "the shim claim table is not restated from the bindings"
perl -0ne 'exit(!/function externalSurfaceTick\(\) \{[\s\S]*?reconcileCcClaim_\(\);[\s\S]*?ccMap\.tick\(\);/)' "$ui" \
  || fail "the per-tick reconcile does not restate the claim table and flush the CC map"
grep -q '"host_cc_claim_set", JS_NewCFunction' src/shadow/shadow_ui.c || fail "host_cc_claim_set is not bound"
grep -q '"host_cc_learn", JS_NewCFunction' src/shadow/shadow_ui.c || fail "host_cc_learn is not bound"
echo "PASS: cc claim table, the shim walk consults it after the surface claim, and the UI feeds and restates it"
