#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_chain_json_null_section"
mkdir -p "$(dirname "$bin")"

# chain_internal.h includes <malloc.h>, absent on macOS — provide a stub so the
# test compiles on the dev host as well as Linux/CI.
stub="$(mktemp -d)"
trap 'rm -rf "$stub"' EXIT
printf '#include <stdlib.h>\n' > "$stub/malloc.h"

cc -std=gnu11 -Wall -I"$stub" -Isrc -Isrc/host \
  tests/host/test_chain_json_null_section.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin"

"$bin"
