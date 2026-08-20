#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Generated MIDI (an arp's tick output) must travel the rest of the chain.
# See tests/host/test_chain_midi_chain.c for what each case pins.

bin="build/tests/test_chain_midi_chain"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/modules/chain/dsp \
  tests/host/test_chain_midi_chain.c \
  -o "$bin"

"$bin"
