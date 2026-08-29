#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Two halves:
#
#   RUN   chain_macro_apply() out of the real chain_mod.c / chain_params.c
#         against a fake fx plugin (tests/host/test_chain_macro_apply.c) --
#         proves one knob position fans out to N targets with independent
#         signed depth, that an unresolvable row is dropped without blocking
#         its siblings, and that the raw knob position is clamped to [0,1].
#
#   PIN   that the knob-CC dispatch paths (relative, absolute, Shift+Knob
#         adjust) and the patch-load reseed actually call it. chain_midi.c /
#         chain_host.c / chain_patch.c cannot be compiled natively (they
#         dlopen plugins and own the whole get_param/set_param surface), so
#         the wiring is checked at the source level instead.

fail() { echo "FAIL: $1"; exit 1; }

# ------------------------------------------------------------------ run half
bin="build/tests/test_chain_macro_apply"
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
  tests/host/test_chain_macro_apply.c \
  src/modules/chain/dsp/chain_mod.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin"

"$bin"

# ------------------------------------------------------------------ pin half

command grep -q 'chain_macro_apply(inst, i);' src/modules/chain/dsp/chain_midi.c \
    || fail "relative CC 71-78 macro branch no longer calls chain_macro_apply"
n=$(command grep -c 'chain_macro_apply(inst, i);' src/modules/chain/dsp/chain_midi.c || true)
[ "$n" -eq 2 ] || fail "chain_midi.c calls chain_macro_apply $n times, expected 2 (relative + absolute knob paths)"

command grep -q 'chain_macro_apply(inst, found);' src/modules/chain/dsp/chain_host.c \
    || fail "macro_N_row_R:* config writes no longer push the effect immediately"
command grep -q 'chain_macro_apply(inst, i);' src/modules/chain/dsp/chain_host.c \
    || fail "knob_N_adjust (Shift+Knob) macro branch no longer calls chain_macro_apply"

command grep -q 'chain_macro_apply(inst, i);' src/modules/chain/dsp/chain_patch.c \
    || fail "patch load no longer reseeds macro targets after copying knob_mappings"

# Demotion/removal must clear ALL of a macro's chain_mod sources, not just the
# one row a direct-mode 1:1 struct would have had.
command grep -q 'chain_macro_clear_all_sources(inst, found);' src/modules/chain/dsp/chain_host.c \
    || fail "knob_N_set (demote-to-direct) no longer clears the macro's chain_mod sources"
n=$(command grep -c 'chain_macro_clear_all_sources(inst,' src/modules/chain/dsp/chain_host.c || true)
[ "$n" -ge 3 ] || fail "chain_host.c calls chain_macro_clear_all_sources $n times, expected at least 3 (knob_N_set demote, knob_N_clear, knob_N_to_macro re-promote)"

# A chain-shape edit (fx:insert/remove/move) must retarget macro rows the same
# way it already retargets mod_targets/LFOs/direct knob mappings, or a macro's
# stored row desyncs from the position mod_targets[] already renamed.
command grep -q 'k->is_macro' src/modules/chain/dsp/chain_reorder.c \
    || fail "chain_perm_retarget_all no longer branches on macro mappings -- shape edits will orphan macro rows"

echo "PASS: macro knob-position fan-out is wired at every value-change and shape-edit site"
