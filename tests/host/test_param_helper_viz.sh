#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_param_helper_viz"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Isrc -Isrc/host \
  tests/host/test_param_helper_viz.c \
  -o "$bin"

"$bin"
