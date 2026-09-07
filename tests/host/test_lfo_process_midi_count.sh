#!/usr/bin/env bash
#
# lfo_process_midi loops the count it is given, not the global LFO_COUNT.
# See tests/host/test_lfo_process_midi_count.c for why that distinction is
# load-bearing the moment a slot has more routes than Master FX.
set -euo pipefail

cd "$(dirname "$0")/../.."
make -s -C tests/host ../../build/tests/host/test_lfo_process_midi_count >/dev/null
exec ./build/tests/host/test_lfo_process_midi_count
