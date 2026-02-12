#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! command -v perl >/dev/null 2>&1; then
  echo "perl is required to run this test" >&2
  exit 1
fi

# Trigger enums should support a down-turn reset while latched so the next
# up-turn can fire again immediately.
if ! perl -0ne 'exit((/if\s*\(latched\s*&&\s*delta\s*<\s*0\)\s*\{[^}]*accum\s*=\s*0\s*;[^}]*latched\s*=\s*false\s*;/s) ? 0 : 1)' "$file"; then
  echo "FAIL: updateTriggerEnumAccum() is missing latched negative-turn reset logic in ${file}" >&2
  exit 1
fi

echo "PASS: trigger enum latched state supports negative-turn reset"
exit 0
