#!/usr/bin/env bash
set -euo pipefail

file="src/shared/store_utils.mjs"

if [ ! -f "$file" ]; then
  echo "FAIL: Missing $file" >&2
  exit 1
fi

if ! command -v rg >/dev/null 2>&1; then
  echo "FAIL: rg is required to run this test" >&2
  exit 1
fi

start=$(rg -n "const release = fetchReleaseJson\\(" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${start}" ]; then
  echo "FAIL: Could not locate release fetch loop in $file" >&2
  exit 1
fi

ctx=$(sed -n "${start},$((start + 60))p" "$file")

if ! echo "$ctx" | rg -q "if \(mod\.github_repo && mod\.asset_name\)"; then
  echo "FAIL: Missing per-module fallback guard (mod.github_repo && mod.asset_name)" >&2
  exit 1
fi

if ! echo "$ctx" | rg -q 'releases/latest/download/\$\{mod\.asset_name\}'; then
  echo "FAIL: Missing fallback download_url construction from catalog asset_name" >&2
  exit 1
fi

echo "PASS: release.json failure path builds fallback download_url from catalog asset_name"
exit 0
