#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_fx_midi_filter"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Isrc/host \
  tests/host/test_fx_midi_filter.c \
  -o "$bin"

"$bin"
