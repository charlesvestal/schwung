#!/usr/bin/env bash
#
# The mod-route source arithmetic, run natively. See src/host/mod_src.h.
set -euo pipefail

cd "$(dirname "$0")/../.."
make -s -C tests/host ../../build/tests/host/test_mod_src >/dev/null
exec ./build/tests/host/test_mod_src
