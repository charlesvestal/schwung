#!/usr/bin/env bash
#
# mod_tick DISPATCHES ON THE SOURCE, and the LFO path is unchanged.
#
# The trap this pins: a non-LFO route must not advance phase. It is not only a
# wasted sinf per block per route for a number nothing reads -- a route later
# switched back to LFO would have had its phase running the whole time, so the
# waveform resumes from somewhere arbitrary instead of where the user left it.
#
# The second trap is the slew SEED. Switching a route from Velocity to Pressure
# must start from the new source, not glide there from the old one, which sounds
# like a fault in the synth rather than a transition in the matrix.
set -euo pipefail

cd "$(dirname "$0")/../.."
F="src/modules/chain/dsp/chain_host.c"
[ -f "$F" ] || { echo "FAIL: missing $F"; exit 1; }

body=$(/usr/bin/sed -n '/^static void mod_tick(chain_instance_t \*inst, int frames) {/,/^}$/p' "$F")
[ -n "$body" ] || { echo "FAIL: could not lift mod_tick()"; exit 1; }

fail=0
say_fail() { echo "FAIL: $1"; fail=1; }
say_ok()   { echo "  ok  $1"; }

# ---- it dispatches, and the MIDI sources are wired in -------------------
for sym in mod_src_is_lfo mod_src_signal mod_src_slew; do
  if printf '%s' "$body" | /usr/bin/grep -q "$sym"; then
    say_ok "mod_tick calls $sym"
  else
    say_fail "mod_tick never calls $sym"
  fi
done

# ---- phase advance is INSIDE the LFO branch -----------------------------
# Walk the lifted body: everything between the mod_src_is_lfo test and the
# matching else is the LFO arm. lfo_advance_phase outside it means every
# velocity route is paying for, and corrupting, an oscillator it does not use.
outside=$(printf '%s' "$body" | /usr/bin/awk '
  /mod_src_is_lfo\(/            { in_lfo = 1 }
  /^        } else \{/          { in_lfo = 0 }
  /lfo_advance_phase|lfo_compute_shape|lfo_synced_phase/ { if (!in_lfo) print "yes" }
' | /usr/bin/head -1)
if [ -n "$outside" ]; then
  say_fail "phase or waveform work runs OUTSIDE the LFO branch"
else
  say_ok "phase and waveform work only happen for LFO routes"
fi

# ---- mod-to-mod covers all eight, via the one parser --------------------
if printf '%s' "$body" | /usr/bin/grep -qE "target\[3\] >= .1. && .*target\[3\] <= .2."; then
  say_fail "mod-to-mod still hardcodes a 1..2 range"
else
  say_ok "mod-to-mod is not pinned to two routes"
fi
if printf '%s' "$body" | /usr/bin/grep -q 'chain_mod_route_index'; then
  say_ok "mod-to-mod reuses the one index parser"
else
  say_fail "mod-to-mod parses its own index; reuse chain_mod_route_index"
fi

# ---- the send path still bypasses the bus -------------------------------
# It must stay an OFFSET: buses:main_send<N> is read back by saveSendLevels(),
# so writing the modulated value would persist it as the user's level.
if printf '%s' "$body" | /usr/bin/grep -q 'main_send_mod'; then
  say_ok "the send-amount offset path survives"
else
  say_fail "main_send_mod is gone; sends would be written destructively"
fi

# ---- the slew is seeded, not ramped, after a src change -----------------
if printf '%s' "$body" | /usr/bin/grep -q 'slew_primed'; then
  say_ok "the slew is seeded via slew_primed"
else
  say_fail "no slew_primed seeding; a src change glides from the old source"
fi
# ...and set_param must clear the flag, or the seed never re-arms.
if /usr/bin/grep -q 'slew_primed = 0;' src/modules/chain/dsp/chain_mod_routes.c; then
  say_ok "changing src re-arms the seed"
else
  say_fail "changing src does not clear slew_primed"
fi

# ---- nothing realtime-unsafe crept into the per-block path --------------
for bad in unified_log fprintf fopen malloc 'access('; do
  if printf '%s' "$body" | /usr/bin/grep -q -- "$bad"; then
    say_fail "mod_tick calls $bad -- it runs on the SPI callback"
  fi
done
say_ok "mod_tick does no I/O, logging or allocation"

[ "$fail" = "0" ] && echo "ALL PASS" || { echo "FAIL"; exit 1; }
