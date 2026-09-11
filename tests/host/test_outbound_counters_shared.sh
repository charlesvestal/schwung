#!/usr/bin/env bash
# THE OUTBOUND COUNTERS MUST BE SHARED, NOT PER-TRANSLATION-UNIT.
#
# ui_midi_out_carry.h declares its counters `static`, which is right for a
# header full of inline helpers and fatal for a counter: every translation unit
# including it gets ITS OWN COPY. shadow_midi.c (which drains) incremented its
# copies while shim_worker.c (which logs) read its own, so every one of them
# reported zero forever.
#
# That is worse than no instrument. On 2026-09-11 it produced three consecutive
# "clean" windows -- zero foreign packets, zero stranded, zero placed -- during
# a capture where the screen was visibly garbling, and very nearly became the
# conclusion "our side is provably clean, report it to the vendor".
#
# So each counter is published through a real global, the way
# shim_ui_midi_out_drops already was, and this fails if a new one is added
# without doing the same.
set -euo pipefail
cd "$(dirname "$0")/../.."

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

for name in placed stranded foreign drops; do
    g="shim_ui_midi_out_${name}"
    grep -q "extern volatile int ${g};" src/host/shim_worker.h \
      || fail "${g} is not declared in shim_worker.h -- the logger cannot see it"
    grep -q "^volatile int ${g} = 0;" src/schwung_shim.c \
      || fail "${g} has no definition -- it is a per-TU static and always reads zero"
    grep -q "${g} = " src/host/shadow_midi.c \
      || fail "${g} is never published from the TU that drains, so it stays zero"
done

# And the logger must READ the global, not the header's static.
for fn in placed_count stranded_count foreign_count; do
    if grep -q "ui_midi_carry_${fn}()" src/host/shim_worker.c; then
        fail "shim_worker.c reads ui_midi_carry_${fn}() -- that is its OWN zeroed copy of a header static"
    fi
done

[ "$fails" -eq 0 ] || { echo "$fails check(s) failed" >&2; exit 1; }
echo "PASS: outbound counters are shared globals, not per-TU statics"
