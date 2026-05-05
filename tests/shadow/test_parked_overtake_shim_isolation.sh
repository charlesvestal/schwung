#!/usr/bin/env bash
# Wrapper to match repo idiom (.sh tests). Runs the Node test that exercises
# parked-overtake shim isolation against the chain-edit param-shim mutators.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec node "$SCRIPT_DIR/test_parked_overtake_shim_isolation.mjs"
