#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
docs="docs/MODULES.md"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Each unit must appear at least once in the docs
for unit in dB Hz ms sec '%' st BPM; do
  # Use word-boundary check to avoid spurious "ms" inside other words. Use literal grep.
  if ! rg -F -q "\`$unit\`" "$docs"; then
    echo "FAIL: docs/MODULES.md does not document unit \`$unit\`" >&2
    exit 1
  fi
done

# Acceleration must be referenced
if ! rg -F -q 'knob_engine' "$docs"; then
  echo "FAIL: docs/MODULES.md does not mention knob_engine" >&2
  exit 1
fi

if ! rg -F -q 'staleness' "$docs"; then
  echo "FAIL: docs/MODULES.md does not mention staleness reset" >&2
  exit 1
fi

if ! rg -F -q 'param_format' "$docs"; then
  echo "FAIL: docs/MODULES.md does not mention param_format" >&2
  exit 1
fi

echo "PASS"
