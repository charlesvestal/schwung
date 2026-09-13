#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE TEST BUS MUST REACH THE UI, AND FOR A WHILE IT COULD NOT.
#
# `inject_as_hardware` delivered packets at the top of shim_post_transfer, so
# every post-ioctl consumer saw them -- the gesture decoders, midi_monitor, the
# held-step tracker. But `shadow_forward_midi()`, which is the shadow UI's ONLY
# feed, runs in shim_pre_transfer, BEFORE the ioctl (the hardware clears the
# mailbox during the transaction). So injected input drove the shim and moved
# nothing on screen.
#
# Measured: injected step notes updated `held_step` while injected jog turns
# and clicks did nothing at all, in the same run. That reads as "the UI ignores
# injected input" and is really "the UI was fed earlier in the frame".
#
# The fix is two passes over one queue: the PRE half PEEKS (into the shadow
# mailbox, which is what the forward reads) and the POST half POPS (into both,
# which is what the shim's scans read). What this pins is that shape, because
# collapsing it back to one pass silently un-drives every UI test written after
# it.

fail() { echo "FAIL: $1"; exit 1; }
src=src/schwung_shim.c

body=$(awk '/^static void shim_inject_as_hardware/,/^}/' "$src")
[ -n "$body" ] || fail "could not find shim_inject_as_hardware in $src"

echo "$body" | grep -q 'pop ? 0 : n' \
  || fail "the popping pass must take the head each time and the peeking pass must walk forward, or one packet is delivered four times"
echo "$body" | grep -q 'if (pop) shadow_midi_inject_pop' \
  || fail "only ONE of the two passes may consume, or the UI and the shim cannot both see the same packet"

# Both call sites, and the pre one must come BEFORE the forward it exists to feed.
pre=$(awk '/^static void shim_pre_transfer/,/^}/' "$src")
echo "$pre" | grep -q 'shim_inject_as_hardware(shadow, hardware_mmap_addr, 0)' \
  || fail "shim_pre_transfer must deliver the PEEK pass -- without it no injected input reaches any screen"
line_inject=$(echo "$pre" | grep -n 'shim_inject_as_hardware' | head -1 | cut -d: -f1)
line_fwd=$(echo "$pre" | grep -n 'shadow_forward_midi()' | head -1 | cut -d: -f1)
[ -n "$line_inject" ] && [ -n "$line_fwd" ] || fail "could not locate both the injection and the forward in shim_pre_transfer"
[ "$line_inject" -lt "$line_fwd" ] \
  || fail "the injection must run BEFORE shadow_forward_midi -- after it, the UI sees the packet a frame late or never"

# By line number against the function's own start, because shim_post_transfer
# is long enough that an awk range ending at the first column-zero brace stops
# short of it -- a matcher that cannot see the code it guards is worse than no
# guard, which this file has cost the project before.
post_start=$(grep -n '^static void shim_post_transfer' "$src" | cut -d: -f1)
pop_line=$(grep -n 'shim_inject_as_hardware(shadow, (uint8_t \*)hw, 1)' "$src" | cut -d: -f1)
[ -n "$post_start" ] && [ -n "$pop_line" ] \
  || fail "shim_post_transfer must deliver the POP pass -- the shim's own decoders read the hardware mailbox"
[ "$pop_line" -gt "$post_start" ] \
  || fail "the POP pass must sit inside shim_post_transfer"

grep -q 'shadow_midi_inject_peek_at' src/host/shadow_midi_inject_writer.h \
  || fail "peek_at is what lets one pass look ahead without consuming"

echo "PASS: inject_as_hardware reaches both halves of the frame"
