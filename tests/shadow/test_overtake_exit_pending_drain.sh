#!/usr/bin/env bash
# Wrapper to match repo idiom (.sh tests). Runs the Node test that exercises
# draining overtakeExitPending on the Tools-shortcut exit path.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec node "$SCRIPT_DIR/test_overtake_exit_pending_drain.mjs"
