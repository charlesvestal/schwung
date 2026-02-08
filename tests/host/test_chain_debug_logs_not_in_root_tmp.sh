#!/usr/bin/env bash
set -euo pipefail

# Chain DSP debug file logs should not default to /tmp on root.

file="src/modules/chain/dsp/chain_host.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if rg -n 'fopen\("/tmp/' "$file" >/dev/null 2>&1; then
  echo "FAIL: chain_host.c still writes debug logs to /tmp" >&2
  exit 1
fi

echo "PASS: chain_host.c does not write debug logs to /tmp"
exit 0
