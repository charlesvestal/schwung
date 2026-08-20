#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_chain_patch_roundtrip"
mkdir -p "$(dirname "$bin")"

# chain_internal.h includes <malloc.h>, absent on macOS -- stub it so the test
# compiles on the dev host as well as Linux/CI. The temp dir also gives the
# test somewhere to write its patch file that is not the device root FS.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"

# The caps come from the shipped header, never a copy: the point of the test is
# that the patch layer tracks whatever MAX_AUDIO_FX / MAX_MIDI_FX currently are.
# -Wno-sign-compare: chain_params.c has pre-existing int/size_t comparisons that
# are not this test's business to fix, and the noise would bury a real warning.
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_chain_patch_roundtrip.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin"

"$bin" "$work"
