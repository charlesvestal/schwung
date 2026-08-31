#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Two halves (#276 — knobs read back 'dead' under a slot LFO):
#
#   RUN   the real chain_mod.c against a fake synth and prove a plain read of
#         an actively modulated key answers the BASE the user set, while
#         ':effective' serves the driven value
#         (tests/host/test_chain_mod_plain_read_base.c).
#
#   PIN   that chain_host.c's three get_param branches actually route through
#         the two helpers, and that the two UIs which deliberately display the
#         driven value ask for ':effective' by name. chain_host.c cannot be
#         compiled natively — it is the TU that dlopens plugins — so the
#         wiring is checked at the source level instead.

fail() { echo "FAIL: $1"; exit 1; }

# ------------------------------------------------------------------ run half
bin="build/tests/test_chain_mod_plain_read_base"
mkdir -p "$(dirname "$bin")"

# chain_internal.h includes <malloc.h>, which is glibc-only; one shim header
# lets this compile on macOS too and changes nothing about the code under test.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_chain_mod_plain_read_base.c \
  src/modules/chain/dsp/chain_mod.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin"

"$bin"

# ------------------------------------------------------------------ pin half
C=src/modules/chain/dsp/chain_host.c

# Every kind of position — synth (literal, there is one), fx and midi_fx (via
# the id derived from the parsed index, same shape test_chain_modulated_suffix
# pins) — must intercept the plain read and serve ':effective'.
for helper in chain_mod_get_base_for_plain_key chain_mod_get_effective_for_subkey; do
  command grep -q "$helper(inst, \"synth\", subkey, buf, buf_len)" "$C" \
    || fail "get_param synth branch does not call $helper"
  for var in fx_id mfx_id; do
    command grep -q "$helper(inst, $var, subkey, buf, buf_len)" "$C" \
      || fail "get_param $var branch does not call $helper"
  done
done

# The two UIs that display the driven value ask for it by name — reading the
# plain key there would now show the base and freeze the dot on the pointer.
command grep -q 'fullKey(key) + ":effective"' src/shared/param_pages/page_controller.mjs \
  || fail "the grid dot does not read :effective (page_controller.mjs)"
command grep -qF ':effective`' src/shadow/shadow_ui.js \
  || fail "the wav editor mod marker does not read :effective (shadow_ui.js)"

echo "PASS: plain reads answer the base while modulated, and the dot reads :effective"
