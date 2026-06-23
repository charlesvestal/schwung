#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_corun_cede_default"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Isrc/host \
  tests/host/test_corun_cede_default.c \
  -o "$bin"

"$bin"
