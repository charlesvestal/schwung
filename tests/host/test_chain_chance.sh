#!/usr/bin/env bash
# Step chance through the chain's entry points (tests/host/test_chain_chance.c),
# and the one fact only the source can show: the gate runs in v2_on_midi AHEAD
# of the LFO retrigger, the MIDI FX and the synth -- a dropped note must not
# retrigger an LFO or feed an arpeggiator.
set -e
cd "$(dirname "$0")/../.."
fail() { echo "FAIL: $1"; exit 1; }

bin="build/tests/test_chain_chance"
mkdir -p "$(dirname "$bin")"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_chain_chance.c src/modules/chain/dsp/chain_chance.c -lm -o "$bin"
"$bin"

f=src/modules/chain/dsp/chain_midi.c
gate=$(grep -n 'if (!chance_filter(inst, msg, len, source)) return;' "$f" | head -1 | cut -d: -f1)
lfo=$(grep -n 'lfo_process_midi(inst->lfos, msg, len);' "$f" | head -1 | cut -d: -f1)
[ -n "$gate" ] || fail "v2_on_midi no longer calls chance_filter"
[ -n "$lfo" ] || fail "cannot find the LFO retrigger in v2_on_midi"
[ "$gate" -lt "$lfo" ] || fail "chance_filter runs AFTER the LFO retrigger (line $gate vs $lfo)"
grep -q 'chain_set_clip_pass' src/host/shadow_chain_mgmt.c || fail "the shim never dlsyms chain_set_clip_pass"
echo "PASS: chance gate sits ahead of LFO/MIDI FX/synth"
