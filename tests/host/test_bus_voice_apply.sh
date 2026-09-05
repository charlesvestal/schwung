#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_bus_voice_apply"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host \
  tests/host/test_bus_voice_apply.c \
  -o "$bin"

"$bin"
