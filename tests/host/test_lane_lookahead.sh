#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
bin="build/tests/host/test_lane_lookahead"
mkdir -p "$(dirname "$bin")"
cc -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host tests/host/test_lane_lookahead.c -o "$bin" -lm
"$bin"
