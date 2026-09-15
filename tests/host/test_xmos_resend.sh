#!/usr/bin/env bash
# The XMOS control-message re-send state machine, replayed against the failure
# captured on hardware 2026-09-12. See src/host/xmos_resend.h.
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_xmos_resend"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Isrc/host \
  tests/host/test_xmos_resend.c \
  -o "$bin"

"$bin"
