#!/usr/bin/env bash
set -euo pipefail

# Test: Verify hotkey detection reads raw MIDI and shift CC is not filtered
# Hotkey detection uses the unfiltered MIDI_IN buffer, and the post-ioctl
# filter should only remove jog/back/knobs (not shift CC 0x31).

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Verify hotkey detection uses raw MIDI_IN buffer.
hotkey_start=$(rg -n "void midi_monitor\\(\\)" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${hotkey_start}" ]; then
  echo "FAIL: Could not locate midi_monitor() in ${file}" >&2
  exit 1
fi

hotkey_block=$(sed -n "${hotkey_start},$((hotkey_start + 40))p" "$file")
if ! echo "${hotkey_block}" | rg -q "uint8_t \\*src = global_mmap_addr \\+ MIDI_IN_OFFSET;"; then
  echo "FAIL: midi_monitor() does not read raw MIDI_IN buffer" >&2
  exit 1
fi

# Check the post-ioctl MIDI_IN filter block does not include shift CC.
filter_start=$(rg -n "Filter MIDI_IN: zero out jog/back/knobs" "$file" | head -n 1 | cut -d: -f1 || true)
filter_end=$(rg -n "Note messages: filter knob touches" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${filter_start}" ] || [ -z "${filter_end}" ]; then
  echo "FAIL: Could not locate post-ioctl MIDI_IN filter block in ${file}" >&2
  exit 1
fi

filter_block=$(sed -n "${filter_start},${filter_end}p" "$file")
if ! echo "${filter_block}" | rg -q "CC_JOG_WHEEL"; then
  echo "FAIL: Post-ioctl filter block missing jog CC filter" >&2
  exit 1
fi
if ! echo "${filter_block}" | rg -q "CC_KNOB1"; then
  echo "FAIL: Post-ioctl filter block missing knob CC filter" >&2
  exit 1
fi
if echo "${filter_block}" | rg -q "0x31|CC_SHIFT"; then
  echo "FAIL: Shift CC appears in post-ioctl filter block" >&2
  exit 1
fi

echo "PASS: Hotkey reads raw MIDI_IN and shift CC is not filtered"
exit 0
