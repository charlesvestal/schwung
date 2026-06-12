#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

f="src/schwung_shim.c"

# MIDI_IN events are 8 bytes (4-byte USB-MIDI packet + 4-byte timestamp).
# Stride-4 scans decode timestamp dwords as packets: a timestamp LSB of 46
# reads as a cable-2 pitchbend header (observed live: "2e 00 00 00" ghost
# events in spi_midi_log), and large timestamps can forge valid-looking
# events deep into a session. All MIDI_IN scans must stride 8 and treat a
# zero header as an empty slot (continue — never break: the filter zeroes
# individual slots mid-run, so events after a filtered slot are valid).

# 1. No stride-4 loops over MIDI_BUFFER_SIZE remain (the legitimate 4-byte
#    buffers — the UI SHM ring and the 80-byte MIDI_OUT region — use their
#    own bounds/index names).
if rg -q 'MIDI_BUFFER_SIZE; [ij] \+= 4' "$f"; then
  echo "FAIL: stride-4 MIDI_IN scan(s) remain:" >&2
  rg -n 'MIDI_BUFFER_SIZE; [ij] \+= 4' "$f" >&2
  exit 1
fi

# 2. The remap/BLOCK scanners must skip empty slots, not break — breaking
#    on the first filtered (zeroed) slot missed later cable-2 events.
for fn in shim_remap_cable2_channels shim_block_cable2_in_sh_midi; do
  body=$(awk "/^static void ${fn}/,/^}/" "$f")
  if grep -q 'break;.*end' <<<"$body" || grep -Eq 'if \(header == 0\) break|== 0\) break' <<<"$body"; then
    echo "FAIL: ${fn} still breaks at the first empty slot" >&2
    exit 1
  fi
done

# 3. The spi_midi_log IN tap must stride 8 (it produced the ghost events).
if rg -q 'for \(int i = 0; i < 248; i \+= 4\)' "$f"; then
  echo "FAIL: spi_midi_log IN tap still strides 4" >&2
  exit 1
fi

# 4. The indicator OUT scan must stay inside the 80-byte MIDI_OUT region.
if awk '/OUT side: cable-2 MIDI_OUT note-ons/,/^        \}/' "$f" | rg -q 'for \(.*MIDI_BUFFER_SIZE'; then
  echo "FAIL: MIDI_OUT indicator scan overruns the 80-byte OUT region" >&2
  exit 1
fi

echo "PASS: all MIDI_IN scans stride 8 with skip-empty-slot semantics"
