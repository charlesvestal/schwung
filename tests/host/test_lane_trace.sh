#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
bin="build/tests/host/test_lane_trace"
mkdir -p "$(dirname "$bin")"
cc -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host tests/host/test_lane_trace.c -o "$bin"
"$bin"
