#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# An override source (an automation lane) sets a target's value outright;
# offsets from chain_mod_emit_value (LFOs) still sum on top; clearing the
# override restores the base. Runs the real chain_mod.c against a fake
# one-param synth (tests/host/test_chain_mod_override.c).

bin="build/tests/test_chain_mod_override"
mkdir -p "$(dirname "$bin")"

# chain_internal.h includes <malloc.h>, which is glibc-only; one shim header
# lets this compile on macOS too and changes nothing about the code under test.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_chain_mod_override.c \
  src/modules/chain/dsp/chain_mod.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin"

"$bin"
