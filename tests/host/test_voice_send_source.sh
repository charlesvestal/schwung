#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_voice_send_source"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host \
  tests/host/test_voice_send_source.c \
  -o "$bin" -lm

"$bin"
