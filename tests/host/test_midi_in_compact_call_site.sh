#!/usr/bin/env bash
# Source pin: the shim must close MIDI_IN gaps, and must do it LAST.
#
# shadow_midi_in_compact() is what stops a filtered (zeroed) slot from acting
# as a terminator that hides every event behind it from Move's firmware — the
# stuck-pad bug. The unit test (test_midi_in_compact.c) proves the function is
# correct; only this pin proves the shim still calls it, and in the one place
# where it is safe to.
#
# "Last" is load-bearing: the blocking sites in shim_post_transfer pair the
# shadow slot `sh[j]` with the hardware slot `hw[j]` by index. Moving the
# compaction above any of them would let indices shift under a live pairing,
# so a block would zero the wrong event.
set -u

SHIM="$(dirname "$0")/../../src/schwung_shim.c"
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

[ -f "$SHIM" ] || { echo "FAIL: cannot find $SHIM" >&2; exit 1; }

# Comment lines are excluded. A doc comment that NAMES this function --
# midi_in_swallow's does, because the pairing it performs is only safe while
# compaction runs last -- was being counted as a second call site, which both
# failed the count and made `compact_line` point at prose. A test that a
# correct comment can break gets edited into uselessness the first time that
# happens.
strip_comments() { grep -vE '^\s*(/\*|\*|//)' "$1"; }

calls=$(strip_comments "$SHIM" | grep -c 'shadow_midi_in_compact(')
[ "$calls" -eq 1 ] || fail "expected exactly 1 call to shadow_midi_in_compact, found $calls"

compact_line=$(grep -nE '^[^*/]*shadow_midi_in_compact\(' "$SHIM" | head -1 | cut -d: -f1)
[ -n "$compact_line" ] || fail "shim never calls shadow_midi_in_compact — a filtered slot again hides the events behind it"

# Every write that zeroes a shadow MIDI_IN slot must happen BEFORE the
# compaction, or its gap survives into Move's reader.
if [ -n "${compact_line:-}" ]; then
    # Both the raw form and the helper that replaced most of it. The helper's
    # own body zeroes slots and is DEFINED above shim_post_transfer, so it is
    # not a call site and must not be measured as one -- only its callers are.
    last_zero=$(grep -n -E '^\s*(sh|sh_midi|src)\[j\] = 0;|^\s*midi_in_swallow\(' "$SHIM" \
        | grep -v 'shadow_midi_in\[j\]' | tail -1 | cut -d: -f1)
    [ -n "$last_zero" ] || fail "no slot-zeroing sites found — has the filter moved?"
    if [ -n "$last_zero" ] && [ "$last_zero" -gt "$compact_line" ]; then
        fail "a MIDI_IN slot is zeroed at line $last_zero, AFTER the compaction at $compact_line — that gap reaches Move"
    fi
fi

# The 8-byte-stride MIDI_IN walks must stop at 248, not at MIDI_BUFFER_SIZE
# (256) — the extra iteration reads, and in the filter loop WROTE, the RX
# display-status word that sits immediately behind MIDI_IN.
overrun=$(grep -c -E 'for \(int [ij] = 0; [ij] (\+ 8 )?<=? MIDI_BUFFER_SIZE; [ij] \+= 8\)' "$SHIM")
[ "$overrun" -eq 0 ] || fail "$overrun MIDI_IN loop(s) still bounded by MIDI_BUFFER_SIZE at an 8-byte stride"

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed" >&2
    exit 1
fi
echo "PASS: the shim compacts MIDI_IN, once, after every slot-zeroing site"
