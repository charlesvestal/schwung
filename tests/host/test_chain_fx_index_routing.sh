#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# FX param routing must be INDEXED, not enumerated.
#
# The branches this replaced were copy-paste twins differing only by 0 vs 1,
# over machinery (v2_load_audio_fx_slot(inst, N, ...), fx_smoothers[N]) that
# was already index-generic. Enumerating them again at 8 would be 8x the same
# bug surface.
#
# This is a SOURCE pin — it never runs a line of chain_host.c. The parser's
# behaviour is covered by tests/host/test_chain_key_index.sh, which compiles
# and runs it.

C=src/modules/chain/dsp/chain_host.c
H=src/modules/chain/dsp/chain_internal.h
fail() { echo "FAIL: $1"; exit 1; }

# Read the caps rather than restating them, so a later bump does not have to
# edit this file — same policy as test_chain_key_index.sh, which drives the
# parser at whatever the header says.
cap_fx=$(awk '/^#define MAX_AUDIO_FX /{print $3}' "$H")
cap_midi=$(awk '/^#define MAX_MIDI_FX /{print $3}' "$H")
[ -n "$cap_fx" ] || fail "could not read MAX_AUDIO_FX from $H"
[ -n "$cap_midi" ] || fail "could not read MAX_MIDI_FX from $H"
[ "$cap_fx" -ge 8 ] || fail "MAX_AUDIO_FX is $cap_fx, want at least 8"
[ "$cap_midi" -ge 8 ] || fail "MAX_MIDI_FX is $cap_midi, want at least 8"

command grep -q 'chain_fx_index_from_key' "$C" || fail "no indexed router"

# Every enumerated form that was collapsed. One of these coming back as a
# quick special case is how the twins grow back: the generic branch still
# handles the key, so the special case is invisible until the two disagree.
forbidden=(
    'strncmp(key, "fx1:", 4)'
    'strncmp(key, "fx2:", 4)'
    'strncmp(key, "midi_fx1:", 9)'
    'strncmp(key, "midi_fx2:", 9)'
    'strcmp(key, "fx1:bypassed")'
    'strcmp(key, "fx2:bypassed")'
    'strcmp(key, "fx3:bypassed")'
    'strcmp(key, "fx4:bypassed")'
    'strcmp(key, "midi_fx1:bypassed")'
    'strcmp(key, "midi_fx2:bypassed")'
)
for pat in "${forbidden[@]}"; do
    if command grep -qF "$pat" "$C"; then
        fail "'$pat' is enumerated again — route it through chain_fx_index_from_key"
    fi
done

echo "PASS: FX routing is indexed and the caps are $cap_fx / $cap_midi"
