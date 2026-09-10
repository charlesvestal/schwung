#!/usr/bin/env bash
#
# IF THE PADS ARE YOURS, SO ARE THEIR LIGHTS.
#
# `pad_block` hands pad presses to whoever is on screen. It said nothing about
# the LEDs, so Move went on lighting the pad of every note sounding on the
# track -- and a MIDI FX plays notes on that track, so its own chords wrote
# their pitches onto the grid over whatever the owner had drawn.
#
# Two properties, and the second is the one that is easy to lose:
#
#   * Move's cable-0 note LED writes for 68-99 are STRIPPED while the block is
#     held, so the owner's picture stands;
#   * they are stripped AFTER the caching scan, so `move_note_led_state` still
#     tracks what Move WANTS -- which is what the release replays. Strip before
#     the scan and the cache goes stale, the release restores a picture from
#     before the block, and the grid comes back wrong.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
F="$ROOT/src/host/shadow_led_queue.c"
fail=0
note(){ echo "FAIL: $1"; fail=1; }

grep -q 'if (ctrl && ctrl->pad_block) {' "$F" \
  || note "the pad LED strip is gone -- Move's note lighting bleeds through the owner's grid again"
grep -q 'd1 < 68 || d1 > 99' "$F" \
  || note "the strip is not bounded to the pad range"

# Order: the caching scan must come FIRST, or the release replays a stale picture.
python3 - "$F" <<'PY' || fail=1
import sys
src = open(sys.argv[1]).read()
scan = src.index("move_note_led_state[d1] = color;")
strip = src.index("if (ctrl && ctrl->pad_block) {")
if not scan < strip:
    print("FAIL: pad LEDs are stripped BEFORE Move's state is cached -- "
          "the release would restore a picture from before the block")
    sys.exit(1)
PY

# And only Move's own writes: cable 0. Cable 2 is external gear, not the grid.
grep -q "(midi_out\[i\] >> 4) & 0x0F) != 0" "$F" \
  || note "the strip is not restricted to cable 0"

[ "$fail" = 0 ] || exit 1
echo "PASS: $(basename "$0")  (a held pad block owns the pad LEDs too)"
