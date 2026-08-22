#!/usr/bin/env bash
# Source pin: WHERE the two audio-FX MIDI guards are applied.
#
# test_fx_midi_filter.c proves the predicates are correct. Only this pin proves
# they are wired to the right places — which is the entire bug, because the
# original defect was not a wrong predicate but three feed sites that each
# grew their own guard (or none) in ignorance of the others.
#
# Two invariants, and they pull in OPPOSITE directions:
#
#   1. The channel filter lives INSIDE shadow_master_fx_forward_midi, so all
#      three feed sites are gated by construction and a fourth cannot be added
#      ungated.
#   2. The pad-range guard is applied ONLY at the cable-0 sites. The other two
#      feeds carry external MIDI, where a note number is a PITCH — range-
#      filtering those to 68-99 would silence five octaves of a keyboard.
#
# A test that only checked "the guard exists somewhere" would pass with the
# pad guard wrongly applied to external MIDI, which is why #2 is asserted as
# an absence.
set -u

ROOT="$(dirname "$0")/../.."
SHIM="$ROOT/src/schwung_shim.c"
MGMT="$ROOT/src/host/shadow_chain_mgmt.c"
SMIDI="$ROOT/src/host/shadow_midi.c"

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

for f in "$SHIM" "$MGMT" "$SMIDI"; do
    [ -f "$f" ] || { echo "FAIL: cannot find $f" >&2; exit 1; }
done

# --- 1. The chokepoint ------------------------------------------------------
# Extract the body of shadow_master_fx_forward_midi and require the channel
# filter in it. Derived from the source rather than a line number so moving
# the function does not silently disable this check.
body=$(awk '/^void shadow_master_fx_forward_midi\(/{f=1} f{print} f&&/^}/{exit}' "$MGMT")
[ -n "$body" ] || fail "shadow_master_fx_forward_midi not found in shadow_chain_mgmt.c"

if [ -n "$body" ]; then
    printf '%s\n' "$body" | command grep -q 'fx_midi_channel_accepts(' \
        || fail "shadow_master_fx_forward_midi does not apply fx_midi_channel_accepts — the listen channel is unenforced at the one point every feed passes through"

    # It must gate the whole function, not one branch: the filter has to sit
    # ahead of the loop that dispatches to positions.
    filter_line=$(printf '%s\n' "$body" | command grep -n 'fx_midi_channel_accepts(' | head -1 | cut -d: -f1)
    loop_line=$(printf '%s\n' "$body" | command grep -n 'for (int i = 0; i < MASTER_FX_SLOTS' | head -1 | cut -d: -f1)
    if [ -n "$filter_line" ] && [ -n "$loop_line" ] && [ "$filter_line" -gt "$loop_line" ]; then
        fail "the channel filter is applied inside/after the dispatch loop, not ahead of it"
    fi
fi

# --- 2. Cable-0 sites carry the pad guard -----------------------------------
# Both cable-0 broadcasts — to Master FX and to slot audio FX — must use the
# pad range. `d1 >= 10` only ever excluded capacitive knob touch; it let step
# buttons (16-31) and track buttons (40-43) through as played notes.
pad_guards=$(command grep -c 'move_surface_note_is_pad(d1)' "$SHIM")
[ "$pad_guards" -ge 2 ] \
    || fail "expected the pad guard on both cable-0 broadcasts in the shim, found $pad_guards"

# The Master FX forward from the shim specifically.
mfx_line=$(command grep -n 'shadow_master_fx_forward_midi(msg, 3, MOVE_MIDI_SOURCE_INTERNAL)' "$SHIM" | head -1 | cut -d: -f1)
if [ -n "$mfx_line" ]; then
    # Look at the few lines above the call for the guard.
    start=$((mfx_line - 6)); [ "$start" -lt 1 ] && start=1
    sed -n "${start},${mfx_line}p" "$SHIM" | command grep -q 'move_surface_note_is_pad(d1)' \
        || fail "the shim's Master FX forward at line $mfx_line is not guarded by move_surface_note_is_pad"
else
    fail "shim no longer forwards MOVE_MIDI_SOURCE_INTERNAL to Master FX — has the site moved?"
fi

# The slot audio-FX broadcast from the shim.
bcast_line=$(command grep -n 'MOVE_MIDI_SOURCE_FX_BROADCAST' "$SHIM" | head -1 | cut -d: -f1)
if [ -n "$bcast_line" ]; then
    start=$((bcast_line - 14)); [ "$start" -lt 1 ] && start=1
    sed -n "${start},${bcast_line}p" "$SHIM" | command grep -q 'move_surface_note_is_pad(d1)' \
        || fail "the shim's slot FX_BROADCAST at line $bcast_line is not guarded by move_surface_note_is_pad"
fi

# And the guard it replaced must be gone from both, or a step button still
# reaches an FX through whichever one kept it.
stale=$(command grep -c 'if (d1 >= 10 && shadow_plugin_v2' "$SHIM" || true)
[ "$stale" -eq 0 ] \
    || fail "$stale FX broadcast(s) still gated by 'd1 >= 10' — step and track buttons reach audio FX"

# --- 3. External sites must NOT carry the pad guard -------------------------
# shadow_midi.c feeds Master FX from the MIDI_OUT echo and the cable-2 THRU
# path. Both carry musical pitch.
if command grep -q 'move_surface_note_is_pad' "$SMIDI"; then
    fail "shadow_midi.c applies the cable-0 pad guard to external MIDI — that clamps a keyboard to notes 68-99"
fi

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed" >&2
    exit 1
fi
echo "PASS: channel filter at the chokepoint, pad guard on cable-0 only"
