#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
bin="build/tests/test_lock_step"
mkdir -p "$(dirname "$bin")"
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-variable -Isrc \
  tests/host/test_lock_step.c \
  -lm -o "$bin"
"$bin"
