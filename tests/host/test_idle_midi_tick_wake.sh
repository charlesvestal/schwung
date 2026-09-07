#!/usr/bin/env bash
# The WIRING of the idle-tick wake. The transitions themselves are driven by
# tests/host/test_chain_idle_tick.c against chain_idle_tick.h — this pins only
# what a unit test cannot see: that the three call sites exist, in the right
# files, and in the one order the handshake depends on.
#
# It deliberately does NOT grep for the body of the state machine. The first
# version of this file pinned nine exact source lines, which broke on any
# reformat and proved nothing about behaviour.
set -euo pipefail

cd "$(dirname "$0")/../.."

MIDI=src/modules/chain/dsp/chain_midi.c
HOST=src/modules/chain/dsp/chain_host.c
SHIM=src/schwung_shim.c
HDR=src/modules/chain/dsp/chain_idle_tick.h

fail() { echo "FAIL: $1"; exit 1; }

[ -f "$HDR" ] || fail "the idle-tick transitions are not in a header tests/host can run"

# 1. The tick reports delivery, and reports it from INSIDE the synth-delivery
#    guard. A MIDI FX slot with no synth is silent by construction, so waking
#    on "a message was emitted" switches the idle gate off for the one slot
#    that can never need it.
grep -q 'int v2_tick_midi_fx(chain_instance_t \*inst, int frames)' "$MIDI" \
  || fail "MIDI FX tick does not report whether it delivered"
awk '
  /^int v2_tick_midi_fx/ {fn=1}
  fn && /synth_plugin_v2->on_midi\(inst->synth_instance/ {guard=1}
  fn && guard && /delivered = 1;/ {found=1}
  fn && /^}/ {exit}
  END {exit found ? 0 : 1}
' "$MIDI" || fail "the wake flag is not set inside the synth-delivery guard"

# 2. mod:tick marks, and does so with the tick result — not unconditionally.
grep -q 'chain_idle_tick_mark(&inst->idle_tick, v2_tick_midi_fx(' "$HOST" \
  || fail "mod:tick does not record the MIDI tick result"

# 3. render_block asks before ticking, so a woken block never ticks twice.
grep -q 'if (chain_idle_tick_consume(&inst->idle_tick))' "$HOST" \
  || fail "render_block does not consume the double-tick guard"

# 4. The export the shim resolves by dlsym.
grep -q 'int chain_take_midi_tick_wake(void \*instance)' "$HOST" \
  || fail "chain DSP exports no one-shot MIDI wake result"
grep -q 'chain_take_midi_tick_wake' src/host/shadow_chain_mgmt.c \
  || fail "the shim loader does not resolve the wake export"

# 5. ORDER, in the shim: mod:tick, THEN take, THEN the idle bail-out. take() is
#    one-shot and clears the guard when the answer is no, so asking before the
#    tick, or twice, silently loses the wake.
awk '
  /"mod:tick"/ {tick=NR}
  tick && /shadow_chain_take_midi_tick_wake\(shadow_chain_slots/ {take=NR}
  take && /goto slot_run_deferred_fx;/ {bail=NR; exit}
  END {exit (tick && take && bail && tick < take && take < bail) ? 0 : 1}
' "$SHIM" || fail "the shim does not tick, then take, then bail — in that order"

# 6. A MIDI wake is not a probe. probe_burst_this_frame is the stagger-
#    alignment detector, so the increment must sit in the ELSE arm (the real
#    probe frame). On the wake path it reports a spike the stagger cannot fix.
awk '
  /shadow_chain_take_midi_tick_wake\(shadow_chain_slots/ {take=1; next}
  take && !els && /probe_burst_this_frame\+\+/ {bad=1; exit}
  take && !els && /^                \} else \{/ {els=1; next}
  els && /probe_burst_this_frame\+\+/ {inc=1; exit}
  END {exit (!bad && els && inc) ? 0 : 1}
' "$SHIM" || fail "a MIDI-driven wake is counted as an idle probe"

echo "PASS: idle MIDI FX timers wake the synth without double ticking"
