#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_usbc_emit_gate"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Isrc/host \
  tests/host/test_usbc_emit_gate.c \
  -o "$bin"

"$bin"
