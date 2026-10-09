#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v node >/dev/null || { echo "FAIL: node is required" >&2; exit 1; }
node --test tests/host/test_claims_own_screen.mjs
