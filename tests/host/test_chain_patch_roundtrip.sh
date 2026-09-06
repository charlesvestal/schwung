#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

fail() { echo "FAIL: $1"; exit 1; }

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

# ---------------------------------------------------------------- format pin
#
# The serializer lives in chain_host.c, which dlopens plugins and so cannot be
# compiled natively. What matters about it is a property the parser cannot see:
# an ORDINARY knob must re-save byte-identically to what shipped before
# destinations existed, or every patch file in the field is rewritten the first
# time it is touched. That holds only while the new fields are emitted
# CONDITIONALLY, so the condition is pinned here.
H=src/modules/chain/dsp/chain_host.c
command grep -q 'if (km->dests\[di\].lo != 0.0f || km->dests\[di\].hi != 1.0f)' "$H" \
  || fail "the knob serializer no longer emits lo/hi only for a non-whole-range destination"
command grep -q 'if (multi) {' "$H" \
  || fail "the knob serializer no longer emits dest/pos only for a multi-destination knob"

# ...and a truncated array must not be emitted half-written. JSON.parse in the
# shadow UI throws that into a silent catch, so the knobs would vanish from the
# saved patch with no error anywhere.
command grep -q 'return snprintf(buf, buf_len, "\[\]");' "$H" \
  || fail "the knob serializer no longer answers [] rather than half an array on truncation"
