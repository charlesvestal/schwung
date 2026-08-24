#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."
mkdir -p build/tests
cc -std=c11 -Wall -Wextra -Werror \
  -Isrc -Isrc/host \
  tests/host/test_linein_state.c \
  src/modules/sound_generators/linein/linein.c \
  -lm -o build/tests/test_linein_state
build/tests/test_linein_state
