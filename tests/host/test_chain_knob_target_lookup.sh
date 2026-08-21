#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Two halves:
#
#   RUN   knob_find_param() out of the real chain_params.c and prove fx6 lands
#         on slot 5, that the live count bounds it, and that an empty interior
#         position is a hole (tests/host/test_chain_knob_target_lookup.c).
#
#   PIN   that chain_host.c's knob metadata sites actually delegate to it.
#         chain_host.c cannot be compiled natively -- it is the TU that dlopens
#         plugins and owns the whole get_param/set_param surface -- so the
#         delegation is checked at the source level instead.

fail() { echo "FAIL: $1"; exit 1; }

# ------------------------------------------------------------------ run half
bin="build/tests/test_chain_knob_target_lookup"
mkdir -p "$(dirname "$bin")"

# chain_internal.h includes <malloc.h>, which is glibc-only; one shim header
# lets this compile on macOS too and changes nothing about the code under test.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"

# -Wno-sign-compare: chain_params.c has pre-existing int/size_t comparisons
# that are not this test's business to fix.
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_chain_knob_target_lookup.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin"

"$bin"

# ------------------------------------------------------------------ pin half
C=src/modules/chain/dsp/chain_host.c

# The enumerated forms, verbatim. Any one of these coming back means a knob on
# fx4+ has silently stopped resolving its chain_params metadata again.
forbidden=(
    'strcmp(target, "fx1") == 0 && inst->fx_count > 0'
    'strcmp(target, "fx2") == 0 && inst->fx_count > 1'
    'strcmp(target, "fx3") == 0 && inst->fx_count > 2'
    'strcmp(target, "midi_fx1") == 0 && inst->midi_fx_count > 0'
    'strcmp(target, "midi_fx2") == 0 && inst->midi_fx_count > 1'
)
for pat in "${forbidden[@]}"; do
    if command grep -qF "$pat" "$C"; then
        fail "'$pat' is enumerated again in $C -- parse the index instead"
    fi
done

# Both knob metadata sites (knob_N_set, and the knob_N_<query> getter) must
# ask knob_find_param. Two is the count today; more is fine, fewer is not.
n=$(command grep -c 'knob_find_param(inst, target, param)' "$C" || true)
[ "$n" -ge 2 ] || fail "expected >= 2 knob_find_param call sites in $C, found $n"

# The live-value read in "knob_mappings" is a different shape (it dispatches to
# the plugin, not to metadata) and gets the bare-id parser directly.
command grep -q 'chain_fx_index_from_id(target, "fx", MAX_AUDIO_FX)' "$C" \
    || fail "the knob_mappings live read does not parse an fx index"
command grep -q 'chain_fx_index_from_id(target, "midi_fx", MAX_MIDI_FX)' "$C" \
    || fail "the knob_mappings live read does not parse a midi_fx index"

echo "PASS: chain_host.c knob metadata sites are indexed"
