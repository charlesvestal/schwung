#!/usr/bin/env bash
set -euo pipefail

# A module dropped from a repo's multi-module release.json must be reported
# distinctly (not misattributed to a corrupt "Invalid release.json format"),
# so a removed module surfaces in the log instead of silently freezing at its
# last-known version. Pin the explicit branch and its ordering.

file="src/shared/store_utils.mjs"

if [ ! -f "$file" ]; then
  echo "FAIL: Missing $file" >&2
  exit 1
fi

if ! command -v rg >/dev/null 2>&1; then
  echo "FAIL: rg is required to run this test" >&2
  exit 1
fi

# The explicit removed-module branch: multi-module doc present, this id absent.
removed_line=$(rg -n "no longer published in" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${removed_line}" ]; then
  echo "FAIL: Missing explicit 'no longer published' branch for dropped multi-module entries" >&2
  exit 1
fi

# Its guard must key on the release having a modules map and a requested id.
ctx=$(sed -n "$((removed_line - 3)),${removed_line}p" "$file")
if ! echo "$ctx" | rg -q "if \(release\.modules && module_id\)"; then
  echo "FAIL: 'no longer published' branch not guarded by (release.modules && module_id)" >&2
  exit 1
fi

# It must run BEFORE the generic "Invalid release.json format" catch, otherwise
# a valid multi-module doc missing this id would be misreported as corrupt.
invalid_line=$(rg -n 'console\.log\(.Invalid release\.json format' "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${invalid_line}" ]; then
  echo "FAIL: Could not locate the generic invalid-format guard" >&2
  exit 1
fi
if [ "${removed_line}" -ge "${invalid_line}" ]; then
  echo "FAIL: removed-module branch must precede the generic invalid-format guard" >&2
  exit 1
fi

echo "PASS: dropped multi-module entry is surfaced distinctly before the generic invalid-format path"
exit 0
