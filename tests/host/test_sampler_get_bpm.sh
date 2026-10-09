#!/usr/bin/env bash
# get_bpm() is steady while Move plays: sampler_get_bpm() linked for real with
# shadow_transport.c and fed Move's clock per 128-frame block. See the .c.
set -euo pipefail
cd "$(dirname "$0")/../.."
out="build/tests"
bin="$out/test_sampler_get_bpm"
mkdir -p "$out"
# shadow_sampler.c carries format-truncation warnings that are not this test's
# business, so it is built without -Werror; everything else is strict.
cc -std=gnu11 -D_GNU_SOURCE -w -Isrc -c src/host/shadow_sampler.c -o "$out/test_sampler_get_bpm_sampler.o"
cc -std=c11 -Wall -Wextra -Werror -Isrc \
  tests/host/test_sampler_get_bpm.c \
  src/host/shadow_transport.c \
  "$out/test_sampler_get_bpm_sampler.o" \
  -lm -lpthread -o "$bin"
"$bin"
