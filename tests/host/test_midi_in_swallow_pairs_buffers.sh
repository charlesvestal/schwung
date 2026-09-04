#!/usr/bin/env bash
# Blocking a MIDI_IN event must silence BOTH buffers, or it silences the wrong one.
#
# schwung_shim.c holds two views of MIDI_IN and they have different readers:
#
#   shadow (= global_mmap_addr)  is WHAT MOVE SEES
#   hardware_mmap_addr           is the real mailbox, which Move never reads;
#                                Schwung's own post-ioctl scans read it
#
# Twelve sites meant "block this from reaching Move" and ELEVEN zeroed only the
# hardware mailbox. That does not block Move — and it is worse than a no-op,
# because a zeroed slot is a TERMINATOR, so it hid every event behind it from
# our own readers. Exactly backwards, at every one of them.
#
# It surfaced only when Shift+Delete reached Move and deleted a clip. The other
# ten leaked into Capture, Sample, Back, Jog Click and the arrows — none
# destructive, which is why they went unnoticed for so long and why the broken
# form looked like the house style. That is the reason this is a test and not a
# comment: the next person to add a shortcut will copy whatever is next to it.
set -euo pipefail
cd "$(dirname "$0")/../.."

SHIM=src/schwung_shim.c
fails=0
note() { echo "FAIL: $1"; fails=$((fails+1)); }

grep -qE "static inline void midi_in_swallow\\(" "$SHIM" \
  || note "midi_in_swallow helper is gone — every call site is free to get this wrong again"

# Every raw 4-byte zeroing of a MIDI_IN slot, with the buffer its loop is
# walking. Anything writing the HARDWARE mailbox by hand is the bug.
bad=$(python3 - "$SHIM" <<'PY'
import re, sys
L = open(sys.argv[1]).read().split('\n')
def base(i):
    for k in range(i, 0, -1):
        m = re.search(r'\*\s*src\s*=\s*(\w+)\s*\+\s*MIDI_IN_OFFSET', L[k])
        if m:
            return m.group(1)
    return None
for i, line in enumerate(L):
    if re.search(r'^\s*src\[j\][^;]*= 0;\s*src\[j\s*\+\s*1\]', line):
        b = base(i)
        # `src` pointing at the shadow buffer is already the buffer Move reads;
        # those sites are correct as they stand.
        if b != 'global_mmap_addr':
            print(f"{i+1}: raw zeroing of {b} (Move never reads it)")
PY
)
[ -z "$bad" ] || { echo "$bad"; note "raw hardware-mailbox zeroing — use midi_in_swallow()"; }

# The helper writes both, and writes the SHADOW one. A helper that lost the
# shadow write would silently restore the original bug at all 17 call sites.
body=$(awk '/^static inline void midi_in_swallow\(/,/^}/' "$SHIM")
echo "$body" | grep -q "shadow_midi_in\[j\] = 0;" || note "midi_in_swallow does not zero the shadow buffer"
echo "$body" | grep -q "hw_midi_in\[j\] = 0;"     || note "midi_in_swallow does not zero the hardware mailbox"

# It is actually used. 17 call sites at the time of writing; the floor guards
# against a refactor quietly reverting them to hand-rolled pairs.
uses=$(grep -c "midi_in_swallow(shadow + MIDI_IN_OFFSET, src, j);" "$SHIM" || true)
[ "$uses" -ge 15 ] || note "only $uses call sites use midi_in_swallow (expected >= 15)"

# Compaction must still run LAST. The pairing is index-based, so a slot that
# moved between the two writes would zero one event in one buffer and a
# different one in the other.
grep -q "shadow_midi_in_compact" "$SHIM" || note "shadow_midi_in_compact call is gone"

if [ "$fails" -ne 0 ]; then echo "$fails check(s) failed"; exit 1; fi
echo "PASS test_midi_in_swallow_pairs_buffers ($uses call sites)"
