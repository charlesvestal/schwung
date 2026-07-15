#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
bin="build/tests/test_shadow_transport"
mkdir -p "$(dirname "$bin")"
cc -std=c11 -Wall -Wextra -Werror -Isrc \
  tests/host/test_shadow_transport.c \
  src/host/shadow_transport.c \
  -lm -o "$bin"
"$bin"
