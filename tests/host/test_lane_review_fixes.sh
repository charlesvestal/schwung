#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The automation-lane defects the 1.6 review confirmed (long keys, fx
# remove/move/insert under a lane, undo across a restore, the snapshot's "{}"
# marker, lanes:rev, the permuted undo/stash/journal copies, adoption of a
# driving twin, and `:modulated` under lanes_off). Each is driven against the
# REAL chain_lanes.c / chain_mod.c / chain_reorder.c -- see the header of
# tests/host/test_lane_review_fixes.c for what each one cost.

bin="build/tests/test_lane_review_fixes"
mkdir -p "$(dirname "$bin")"
work="$(mktemp -d "${TMPDIR:-/tmp}/schwung-lane-review.XXXXXX")"
trap 'rm -rf "$work"' EXIT
# chain_internal.h includes glibc's <malloc.h>; a shim lets this build on macOS.
printf '#include <stdlib.h>\n' > "$work/malloc.h"
# Always rebuilt: ExtFS keeps 1 s mtimes, so a make-style freshness check can
# run a stale binary against edited sources.
rm -f "$bin"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_lane_review_fixes.c \
  src/modules/chain/dsp/chain_lanes.c \
  src/modules/chain/dsp/chain_mod.c \
  src/modules/chain/dsp/chain_reorder.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  src/host/lane_store.c \
  src/host/lane_serial.c \
  src/host/lane_edit.c \
  -lm -o "$bin"

"$bin"
