#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The scene evaluator (src/host/scene_morph.h): the morph formula, the verbs,
# and the wire format both the chain host and the shim parse.

bin="build/tests/test_scene_morph"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
  -Isrc/host \
  tests/host/test_scene_morph.c \
  -lm -o "$bin"

"$bin"
