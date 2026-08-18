#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_xmos_audio"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Isrc/host \
  tests/host/test_xmos_audio.c \
  src/host/shadow_xmos_audio.c \
  -o "$bin"

"$bin"
