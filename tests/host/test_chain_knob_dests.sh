#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
fail() { echo "FAIL: $1"; exit 1; }

bin="build/tests/test_chain_knob_dests"
mkdir -p "$(dirname "$bin")"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_chain_knob_dests.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin"

"$bin"

# ------------------------------------------------------------------ pin half
#
# The keys themselves live in chain_host.c, which dlopens plugins and cannot be
# compiled natively. What is pinned is that they exist and that each routes to
# the owner above rather than editing the list in place -- an edit that skipped
# the owner would skip the seed or the immediate apply, and both fail silently.
H=src/modules/chain/dsp/chain_host.c

command grep -q 'strncmp(action, "dest_", 5) == 0' "$H" \
  || fail "the knob_N_dest_M_* key family is gone"
command grep -q 'sscanf(action + 5, "%d_%15s", &dest_num, sub)' "$H" \
  || fail "the destination index is no longer parsed out of the action"

for pair in 'set:knob_dest_point' 'clear:knob_dest_remove' 'range:knob_dest_set_window'; do
  sub="${pair%%:*}"; fn="${pair##*:}"
  command grep -q "strcmp(sub, \"$sub\")" "$H" || fail "knob_N_dest_M_$sub is gone"
  command grep -q "$fn(" "$H" || fail "knob_N_dest_M_$sub no longer routes through $fn()"
done

command grep -q 'strcmp(action, "position") == 0' "$H" \
  || fail "knob_N_position is gone"

# The readbacks the editor and the touched-knob card depend on. The card takes
# only name and value, so a multi-destination knob must be describable through
# those two and NOT need a third read -- test_knob_card_open_budget counts them.
for q in name value dest_count position dests; do
  command grep -q "strcmp(query_param, \"$q\")" "$H" || fail "knob_N_$q readback is gone"
done

echo "PASS: a knob's destination list has one owner, and the keys route to it"
