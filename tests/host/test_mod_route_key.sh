#!/usr/bin/env bash
#
# The mod<N>: / legacy lfo<N>: key parser, run natively. See
# src/host/mod_route_key.h and its three siblings.
set -euo pipefail

cd "$(dirname "$0")/../.."
make -s -C tests/host ../../build/tests/host/test_mod_route_key >/dev/null
exec ./build/tests/host/test_mod_route_key
