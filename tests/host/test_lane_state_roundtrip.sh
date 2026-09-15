#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
bin="build/tests/host/test_lane_state_roundtrip"
mkdir -p "$(dirname "$bin")"
cc -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host tests/host/test_lane_state_roundtrip.c \
  src/host/lane_store.c src/host/lane_serial.c -lm -o "$bin"
"$bin"
