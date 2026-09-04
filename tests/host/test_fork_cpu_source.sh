#!/usr/bin/env bash
# The CPU page's fork panel must take its numbers from the MEASUREMENT, not the
# displayed rows.
#
# ProcessView.Rows has already dropped every process under the 0.5% display
# floor, and a module-forked child rarely carries a name that is always listed -
# the one observed on the device reported as "Audio Main/SPI". Sourcing the
# panel from Rows therefore renders 0% for a value that WAS computed and then
# thrown away, which is the same discarded-read failure this page exists to
# avoid, just wearing a rounding error.
#
# forkedProcsWithCPU is unit-tested, but a pure function cannot police who calls
# it with what. This pins the call site, the way
# test_midi_in_compact_call_site.sh does for its own.
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 1

SRC=schwung-manager/perf.go
fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

[ -f "$SRC" ] || { fail "missing $SRC"; exit 1; }

if ! grep -q 'forkedProcsWithCPU(.*view\.AllPercent)' "$SRC"; then
    fail "forkedProcsWithCPU must be called with view.AllPercent in $SRC"
    echo "  found instead:"
    grep -n 'forkedProcsWithCPU' "$SRC" | sed 's/^/    /'
fi

# Rows must never be turned into a pid->percent map for this purpose.
if grep -q 'range view\.Rows' "$SRC" && \
   grep -A2 'range view\.Rows' "$SRC" | grep -q '\.Percent'; then
    fail "$SRC builds a pid->percent map from view.Rows - that map has already
      lost every process under the display floor"
fi

if [ "$fails" -ne 0 ]; then
    echo "test_fork_cpu_source: $fails failure(s)"
    exit 1
fi
echo "PASS test_fork_cpu_source"
