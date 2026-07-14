#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_midi_net"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
  -DMIDI_NET_TESTING -Isrc -Isrc/host \
  tests/host/test_midi_net.c \
  src/host/midi_net.c \
  src/host/midi_net_ipmidi.c \
  src/host/midi_net_applemidi.c \
  src/host/midi_net_mdns.c \
  src/host/unified_log.c \
  -lpthread -o "$bin"

"$bin"
