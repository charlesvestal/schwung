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

# Every knob path reaches a plugin through it, and the two TUs that cannot be
# compiled natively must not reach one any OTHER way. A path that grew its own
# plugin->set_param call would bypass the modulation bus again and nothing
# would say so, which is precisely how this bug survived.
# chain_midi.c has no legitimate direct plugin write at all, so the whole file
# is the window.
if command grep -qE '(synth_plugin_v2|fx_plugins_v2|midi_fx_plugins)\[?[a-z]*\]?->set_param' \
     src/modules/chain/dsp/chain_midi.c; then
  fail "chain_midi.c writes a plugin parameter directly; a knob path must go through knob_forward_value"
fi

# chain_host.c does have legitimate ones -- the prefixed synth:/fxN:/midi_fxN:
# set_param routes, which update the modulation base themselves. So the window
# is the knob_ branch only, bounded by its own opening line and the next
# same-indent `else if`. Both anchors are REQUIRED: a window that silently
# found nothing would pass this check while examining an empty string.
H=src/modules/chain/dsp/chain_host.c
kstart=$(command grep -n 'else if (strncmp(key, "knob_", 5) == 0)' "$H" | head -1 | cut -d: -f1)
[ -n "$kstart" ] || fail "could not find the knob_ set_param branch in $H"
kend=$(awk -v s="$kstart" 'NR>s && /^    else if \(/ {print NR; exit}' "$H")
[ -n "$kend" ] || fail "could not find the end of the knob_ branch in $H (its next sibling moved)"
[ "$kend" -gt "$kstart" ] || fail "the knob_ branch window in $H is empty"
# kend is the NEXT branch's own line, so the window stops before it.
if sed -n "${kstart},$((kend - 1))p" "$H" \
     | command grep -qE '(synth_plugin_v2|fx_plugins_v2|midi_fx_plugins)\[?[a-z]*\]?->set_param'; then
  fail "the knob_ branch of $H writes a plugin parameter directly, bypassing the modulation bus"
fi

# The relative path delegates the whole turn; the absolute CC path still
# formats and forwards its own value. Both must end at the helper.
command grep -q 'knob_turn(inst, i, ticks,' src/modules/chain/dsp/chain_midi.c \
  || fail "the relative CC path does not delegate to knob_turn"
command grep -q 'knob_set_position(inst, i,' src/modules/chain/dsp/chain_midi.c \
  || fail "the absolute CC path does not delegate to knob_set_position"

echo "PASS: a knob turn edits the base, and every knob path goes through it"
