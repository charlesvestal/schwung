#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Two halves:
#
#   RUN   knob_emit_cc_out() out of the real chain_params.c against a fake host
#         that captures packets (tests/host/test_chain_knob_cc_out.c).
#
#   PIN   that the three places a chain knob's value changes actually call it,
#         and -- the subtle one -- that the inbound ABSOLUTE path deliberately
#         does not. chain_midi.c / chain_host.c / chain_patch.c cannot be
#         compiled natively (they dlopen plugins and own the get_param/set_param
#         surface), so the wiring is checked at the source level instead.

fail() { echo "FAIL: $1"; exit 1; }

# ------------------------------------------------------------------ run half
bin="build/tests/test_chain_knob_cc_out"
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
  tests/host/test_chain_knob_cc_out.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin"

"$bin"

# ------------------------------------------------------------------ pin half

# A knob's value changes from exactly three places. Each owes the controller an
# update, and each has been the silent one at some point.
command grep -q 'knob_emit_cc_out(inst, i);' src/modules/chain/dsp/chain_host.c \
    || fail "Move's own encoder no longer emits -- the knob goes stale on the surface"
command grep -q 'knob_emit_cc_out(inst, i);' src/modules/chain/dsp/chain_midi.c \
    || fail "relative CC 71-78 no longer emits -- the sender never learns where its delta landed"
command grep -q 'knob_emit_cc_out_all(inst);' src/modules/chain/dsp/chain_patch.c \
    || fail "a patch load no longer dumps -- motors keep showing the previous patch"

# THE LOOP GUARD. The inbound absolute path must NOT echo: a controller set
# this value, and one configured to hear its own output (how a motorised unit
# normally runs) would fight the user's fingers mid-turn. chain_midi.c has
# exactly two emits -- both in the RELATIVE branch (direct-mode knobs, and
# (since the macro feature) macro knobs, which echo their own 0..1 position
# the same way). Both absolute-branch cases (direct-mode and macro) instead
# carry a "Deliberately no knob_emit_cc_out() here" comment documenting the
# omission, which is checked below rather than trusting a bare count to mean
# "no new legitimate site was ever added".
n=$(command grep -c 'knob_emit_cc_out(inst' src/modules/chain/dsp/chain_midi.c || true)
[ "$n" -eq 2 ] || fail "chain_midi.c has $n emit sites, expected exactly 2 (direct-mode + macro relative knob paths)"
n=$(command grep -c 'Deliberately no knob_emit_cc_out() here' src/modules/chain/dsp/chain_midi.c || true)
[ "$n" -eq 2 ] || fail "chain_midi.c documents $n no-echo absolute branches, expected 2 (direct-mode + macro)"

# The absolute branch still has to record what the sender already knows, or the
# next genuine change is measured against a stale baseline.
command grep -q 'last_cc_out = msg\[2\]' src/modules/chain/dsp/chain_midi.c \
    || fail "the absolute path no longer records the value the sender already has"

# Off by default. A slot that is not driving a control surface must not add
# eight CC streams to a port shared with whatever else is plugged in.
command grep -q 'if (!inst || !inst->knob_cc_out) return;' src/modules/chain/dsp/chain_params.c \
    || fail "knob_emit_cc_out no longer gates on the per-patch opt-in"

# The host hook has to be assigned. It was NULL for years -- left out of the
# eleven fields shadow_chain_mgmt.c does assign -- which is the whole reason
# the outbound half did not exist.
command grep -q 'shadow_host_api.midi_send_external' src/host/shadow_chain_mgmt.c \
    || fail "shadow_host_api.midi_send_external is unassigned again -- the chain cannot answer"

echo "PASS: chain knob CC out is wired at every value-change site"
