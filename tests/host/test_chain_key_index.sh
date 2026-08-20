#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

hdr=src/modules/chain/dsp/chain_internal.h

# Read the caps out of the shipped header rather than restating them, so the
# cases below test the limits the chain actually runs with.
cap_fx=$(awk '/^#define MAX_AUDIO_FX /{print $3}' "$hdr")
cap_midi=$(awk '/^#define MAX_MIDI_FX /{print $3}' "$hdr")
if [ -z "$cap_fx" ] || [ -z "$cap_midi" ]; then
  echo "FAIL: could not read MAX_AUDIO_FX / MAX_MIDI_FX from $hdr" >&2
  exit 1
fi

bin="build/tests/test_chain_key_index"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/modules/chain/dsp \
  -DTEST_MAX_FX="$cap_fx" -DTEST_MAX_MIDI_FX="$cap_midi" \
  tests/host/test_chain_key_index.c \
  -o "$bin"

"$bin"
