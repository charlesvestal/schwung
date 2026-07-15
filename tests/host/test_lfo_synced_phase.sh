#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
bin="build/tests/test_lfo_synced_phase"
mkdir -p "$(dirname "$bin")"
cc -std=c11 -Wall -Wextra -Werror -Isrc \
  tests/host/test_lfo_synced_phase.c \
  -lm -o "$bin"
"$bin"
