#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A MOD ROUTE, END TO END, THROUGH THE REAL BUS.
#
# Compiled the same way as test_chain_patch_roundtrip.sh, and for the same
# reason: chain_mod.c and chain_mod_routes.c need the whole chain_internal.h,
# so they are #included into the test rather than linked, with chain_params.c
# and chain_json.c supplying the helpers.
#
# See the .c file for what this catches that a source pin cannot -- in short,
# CALL ORDERING, which is how a feature with nine green tasks and ~100 green
# assertions once shipped without functioning.

bin="build/tests/test_mod_route_e2e"
mkdir -p "$(dirname "$bin")"

# chain_internal.h includes <malloc.h>, absent on macOS -- stub it so the test
# compiles on the dev host as well as Linux/CI.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"

# -Wno-sign-compare: chain_params.c has pre-existing int/size_t comparisons that
# are not this test's business to fix, and the noise would bury a real warning.
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_mod_route_e2e.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin" -lm

exec "./$bin"
