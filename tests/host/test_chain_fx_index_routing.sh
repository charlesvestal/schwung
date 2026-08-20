#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# FX param routing must be INDEXED, not enumerated.
#
# The two branches it replaces were copy-paste twins differing only by 0 vs 1,
# over machinery (v2_load_audio_fx_slot(inst, N, ...), fx_smoothers[N]) that
# was already index-generic. Enumerating them again at 8 would be 8x the same
# bug surface.

C=src/modules/chain/dsp/chain_host.c
H=src/modules/chain/dsp/chain_internal.h
fail() { echo "FAIL: $1"; exit 1; }

command grep -q '#define MAX_AUDIO_FX 8' "$H" || fail "MAX_AUDIO_FX is not 8"
command grep -q '#define MAX_MIDI_FX 8'  "$H" || fail "MAX_MIDI_FX is not 8"

command grep -q 'strncmp(key, "fx2:", 4)' "$C" && fail "the fx2: twin is still enumerated"
command grep -q 'chain_fx_index_from_key' "$C" || fail "no indexed router"

echo "PASS: FX routing is indexed and the caps are 8"
