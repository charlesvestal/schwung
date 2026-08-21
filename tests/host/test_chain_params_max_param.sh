#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# See the header comment in test_chain_params_max_param.c.

work="$(mktemp -d "${TMPDIR:-/tmp}/schwung-chain-params-answer.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# chain_internal.h includes <malloc.h>, which is glibc-only. One shim header so
# the file compiles on macOS as well as Linux; it changes nothing about the
# code under test. (Same shim as test_chain_midi_fx_slot.sh.)
mkdir -p "$work/shim"
cat > "$work/shim/malloc.h" <<'EOF'
#include <stdlib.h>
EOF

bin="build/tests/test_chain_params_max_param"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -I"$work/shim" -Isrc -Isrc/modules/chain/dsp -Isrc/host \
  tests/host/test_chain_params_max_param.c src/modules/chain/dsp/chain_params.c src/modules/chain/dsp/chain_json.c -o "$bin"

"$bin"
