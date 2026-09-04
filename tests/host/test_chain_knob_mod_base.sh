#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A chain knob turn is an EDIT of the resting value, not a write past the
# modulation bus.
#
#   RUN   the real knob_forward_value against the real chain_mod.c and a fake
#         synth, and prove a turn on a modulated parameter updates the BASE and
#         survives every later LFO tick — while an unmodulated parameter still
#         goes straight through (tests/host/test_chain_knob_mod_base.c).
#
#   PIN   that knob_forward_value stays the ONE place a chain knob reaches a
#         plugin. The three knob paths — the external CC decode in chain_midi.c,
#         the absolute CC in the same file, and knob_N_adjust in chain_host.c
#         (which is the path the device's own encoders use) — must all go
#         through it, or a fourth route reintroduces the bug for one input
#         method only, which is exactly the kind of asymmetry nobody notices.
#         Neither TU can be compiled natively, so this half is a source pin.

fail() { echo "FAIL: $1"; exit 1; }

# ------------------------------------------------------------------ run half
bin="build/tests/test_chain_knob_mod_base"
mkdir -p "$(dirname "$bin")"

# chain_internal.h includes <malloc.h>, which is glibc-only; one shim header
# lets this compile on macOS too and changes nothing about the code under test.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_chain_knob_mod_base.c \
  src/modules/chain/dsp/chain_mod.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin"

"$bin"

# ------------------------------------------------------------------ pin half

# The fix itself: knob_forward_value consults the bus before writing through.
P=src/modules/chain/dsp/chain_params.c
command grep -q 'chain_mod_is_target_active(inst, target, param)' "$P" \
  || fail "knob_forward_value does not ask whether the target is modulated"
command grep -q 'chain_mod_update_base_from_set_param(inst, target, param, val_str)' "$P" \
  || fail "knob_forward_value does not update the base"

# ...and it does so BEFORE any plugin write, not after one. A write that
# already happened cannot be taken back, and the ordering is the whole fix.
awk '/^void knob_forward_value/,/^}/' "$P" > "$work/kfv.c"
base_line=$(command grep -n 'chain_mod_update_base_from_set_param' "$work/kfv.c" | head -1 | cut -d: -f1)
# '->set_param(' is the plugin write specifically. A bare 'set_param(' also
# matches chain_mod_update_base_from_set_param on the very line being located,
# which made this comparison compare a line with itself.
set_line=$(command grep -n -- '->set_param(' "$work/kfv.c" | head -1 | cut -d: -f1)
[ -n "$base_line" ] && [ -n "$set_line" ] || fail "could not locate both calls in knob_forward_value"
[ "$base_line" -lt "$set_line" ] \
  || fail "knob_forward_value writes the plugin before telling the modulation bus"

# Every knob path reaches a plugin through it. Counted, not merely present:
# a path that grew its own plugin->set_param call would still match a bare
# grep for the helper elsewhere in the file.
for f in src/modules/chain/dsp/chain_midi.c src/modules/chain/dsp/chain_host.c; do
  command grep -q 'knob_forward_value(inst, target, param, val_str)' "$f" \
    || fail "$f does not forward knob values through knob_forward_value"
done

n=$(command grep -c 'knob_forward_value(inst, target, param, val_str)' src/modules/chain/dsp/chain_midi.c)
[ "$n" -eq 2 ] \
  || fail "chain_midi.c has $n knob_forward_value call sites, expected 2 (relative + absolute CC)"

echo "PASS: a knob turn edits the base, and every knob path goes through it"
