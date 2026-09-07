#!/usr/bin/env bash
#
# THE MOD INPUT IS LATCHED AT BOTH SYNTH-FEED PATHS.
#
# Same rule, and the same war story, as synth:last_note. Two paths feed the
# synth: v2_on_midi carries notes a MIDI FX transformed in process_midi, and
# v2_tick_midi_fx carries notes it EMITTED from tick() -- which is what an
# arpeggiator does, swallowing the held note and emitting its pattern on the
# clock. Instrumenting only the first means a velocity route never moves with an
# arp in the slot, and the failure is silent: the route is enabled, aimed, and
# stuck at its rest value with nothing to report.
#
# The adjacency to chain_record_synth_note IS the invariant being pinned. If a
# third synth-feed path ever appears, both latches must arrive together.
set -euo pipefail

cd "$(dirname "$0")/../.."
F="src/modules/chain/dsp/chain_midi.c"
H="src/modules/chain/dsp/chain_host.c"
[ -f "$F" ] && [ -f "$H" ] || { echo "FAIL: missing sources"; exit 1; }

fail=0
say_fail() { echo "FAIL: $1"; fail=1; }
say_ok()   { echo "  ok  $1"; }

# ---- one definition, exactly two call sites -----------------------------
calls=$(/usr/bin/grep -c 'chain_record_mod_input(' "$F" || true)
if [ "$calls" = "3" ]; then
  say_ok "one definition and exactly two call sites"
else
  say_fail "found $calls occurrences of chain_record_mod_input, expected 3"
fi

# ---- both sit beside their last_note sibling ----------------------------
# The latch sits IMMEDIATELY BEFORE its sibling, not after: test_chain_last_note.sh
# requires chain_record_synth_note to be immediately followed by the synth
# on_midi it describes, so anything inserted between them breaks that pin.
paired=$(/usr/bin/grep -A1 'chain_record_mod_input(inst, out_msgs\[i\], out_lens\[i\]);' "$F" \
         | /usr/bin/grep -c 'chain_record_synth_note' || true)
if [ "$paired" = "2" ]; then
  say_ok "both last_note sites are paired with a mod_input latch"
else
  say_fail "$paired of 2 last_note sites are paired; the two must move together"
fi

# ---- realtime: the latch stores and nothing else ------------------------
body=$(/usr/bin/sed -n '/^static inline void chain_record_mod_input/,/^}$/p' "$F")
if [ -z "$body" ]; then
  say_fail "could not lift chain_record_mod_input()"
else
  for bad in unified_log fprintf fopen malloc 'access(' snprintf 'host->log'; do
    if printf '%s' "$body" | /usr/bin/grep -q -- "$bad"; then
      say_fail "chain_record_mod_input calls $bad -- it runs on the SPI callback"
    fi
  done
  say_ok "the latch does no I/O, logging or allocation"

  # The DECODING is tested by behaviour in tests/host/test_mod_src.c --
  # channel aftertouch reading msg[1], a velocity-0 note-on latching nothing,
  # the CC index masked -- because it lives in mod_src.h now and can be RUN.
  # What is left here is the part only a source pin can see: WHERE the latch is
  # called from, and that it stays a store.
  if printf '%s' "$body" | /usr/bin/grep -q 'mod_input_record(&inst->mod_input, msg, len)'; then
    say_ok "the latch delegates to mod_src.h, which tests/host can run"
  else
    say_fail "chain_record_mod_input no longer delegates to mod_input_record"
  fi
fi

# ---- the rest values are seeded, in BOTH places that reset a slot -------
# calloc says velocity 0, and a velocity route on an unplayed slot would pin its
# target to the bottom of its range -- which reads as a dead synth and gets
# blamed on the target rather than on the untouched source.
resets=$(/usr/bin/grep -c 'mod_input_reset(&inst->mod_input)' "$H" || true)
if [ "$resets" = "2" ]; then
  say_ok "mod_input is seeded at instance creation AND on clear"
else
  say_fail "found $resets mod_input_reset calls in chain_host.c, expected 2"
fi

[ "$fail" = "0" ] && echo "ALL PASS" || { echo "FAIL"; exit 1; }
