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

# NEVER `echo "$big" | grep -q` HERE. `grep -q` exits at the first match and
# closes the pipe; shim_pre_transfer's body is 66 KB, past Linux's 64 KB pipe
# buffer, so the writer still has bytes to push and takes SIGPIPE -- under
# `set -o pipefail` that is a FAILING pipeline, and the `||` then reports
# whichever invariant the grep had just CONFIRMED. It read as "the PEEK pass is
# missing" on a tree that has it, and it is invisible on macOS, whose pipe
# buffer grows. A here-string is a temp file, not a pipe, so the reader cannot
# close on the writer.
grep -q 'pop ? 0 : n' <<<"$body" \
  || fail "the popping pass must take the head each time and the peeking pass must walk forward, or one packet is delivered four times"
grep -q 'if (pop) shadow_midi_inject_pop' <<<"$body" \
  || fail "only ONE of the two passes may consume, or the UI and the shim cannot both see the same packet"

# Both call sites, and the pre one must come BEFORE the forward it exists to feed.
pre=$(awk '/^static void shim_pre_transfer/,/^}/' "$src")
grep -q 'shim_inject_as_hardware(shadow, hardware_mmap_addr, 0)' <<<"$pre" \
  || fail "shim_pre_transfer must deliver the PEEK pass -- without it no injected input reaches any screen"
# -m1 rather than `| head -1`, for the same reason: head closes the pipe on grep.
line_inject=$(grep -n -m1 'shim_inject_as_hardware' <<<"$pre" | cut -d: -f1)
line_fwd=$(grep -n -m1 'shadow_forward_midi()' <<<"$pre" | cut -d: -f1)
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
