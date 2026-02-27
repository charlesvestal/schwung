#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Clock output should be enabled for output and disabled for off/input.
if ! rg -q 'strcmp\(mode,\s*"off"\)' "$file"; then
  echo "FAIL: clock mode parser missing off mapping" >&2
  exit 1
fi
if ! rg -q 'strcmp\(mode,\s*"input"\)' "$file"; then
  echo "FAIL: clock mode parser missing input mapping" >&2
  exit 1
fi
if ! rg -q 'strcmp\(mode,\s*"output"\)' "$file"; then
  echo "FAIL: clock mode parser missing output mapping" >&2
  exit 1
fi

echo "PASS: clock mode parser includes off/input/output mapping"
