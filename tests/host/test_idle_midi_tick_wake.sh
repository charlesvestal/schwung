#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

MIDI=src/modules/chain/dsp/chain_midi.c
HOST=src/modules/chain/dsp/chain_host.c
SHIM=src/schwung_shim.c
MGMT=src/host/shadow_chain_mgmt.c
HDR=src/modules/chain/dsp/chain_internal.h

fail() { echo "FAIL: $1"; exit 1; }

grep -q 'int v2_tick_midi_fx(chain_instance_t \*inst, int frames)' "$MIDI" \
  || fail "MIDI FX tick does not report whether it emitted"
grep -q 'if (count > 0) emitted = 1;' "$MIDI" \
  || fail "generated MIDI does not mark the idle slot for wake-up"
grep -q 'inst->midi_tick_wake = v2_tick_midi_fx(inst, frames);' "$HOST" \
  || fail "mod:tick does not capture the MIDI wake result"
grep -q 'inst->idle_tick_advanced = 1;' "$HOST" \
  || fail "the idle tick is not guarded against double advancement"
grep -q 'if (inst->idle_tick_advanced)' "$HOST" \
  || fail "render_block does not consume the double-tick guard"
grep -q 'int chain_take_midi_tick_wake(void \*instance)' "$HOST" \
  || fail "chain DSP exports no one-shot MIDI wake result"
grep -q 'shadow_chain_take_midi_tick_wake' "$MGMT" \
  || fail "the shim loader does not resolve the wake export"
grep -q 'if (midi_wake)' "$SHIM" \
  || fail "the idle gate does not render a block when MIDI wakes it"
grep -q 'int midi_tick_wake;' "$HDR" \
  || fail "the chain instance has no wake state"

echo "PASS: idle MIDI FX timers wake the synth without double ticking"
