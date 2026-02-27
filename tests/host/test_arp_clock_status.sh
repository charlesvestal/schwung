#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_arp_clock_status"
mkdir -p "$(dirname "$bin")"

cc -std=c11 -Wall -Wextra -Werror \
  -Isrc \
  tests/host/test_arp_clock_status.c \
  src/modules/midi_fx/arp/dsp/arp.c \
  -o "$bin"

"$bin"
