#!/usr/bin/env bash
# The shim's TTS client against the real schwung-tts main loop (fake engine),
# two processes over the real /schwung-tts segment. See the .c for what it pins.
set -euo pipefail
cd "$(dirname "$0")/../.."

out="build/tests"
mkdir -p "$out"
CFLAGS="-std=gnu11 -Wall -Wextra -Wno-unused-parameter -Isrc/host"

cc $CFLAGS -o "$out/fake_schwung_tts" \
  src/host/tts_sidecar.c tests/host/fake_tts_engine.c \
  src/host/shadow_shm_util.c src/host/unified_log.c -lpthread -lrt

cc $CFLAGS -o "$out/test_tts_sidecar_roundtrip" \
  tests/host/test_tts_sidecar_roundtrip.c src/host/tts_client.c \
  src/host/shadow_shm_util.c src/host/unified_log.c -lpthread -lrt

FAKE_TTS_LOG="$out/fake_tts_calls.log" \
  "$out/test_tts_sidecar_roundtrip" "$out/fake_schwung_tts"
