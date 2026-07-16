#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_midi_net"
san_bin="build/tests/test_midi_net_sanitized"
mkdir -p "$(dirname "$bin")"

sources=(
  tests/host/test_midi_net.c \
  src/host/midi_net.c \
  src/host/midi_net_ipmidi.c \
  src/host/unified_log.c
)
common=(-std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter
        -DMIDI_NET_TESTING -Isrc -Isrc/host)

cc "${common[@]}" "${sources[@]}" -lpthread -o "$bin"

"$bin"

if [ "${RUN_SANITIZERS:-1}" = "1" ]; then
  cc "${common[@]}" -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined "${sources[@]}" -lpthread -o "$san_bin"
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$san_bin"
fi
