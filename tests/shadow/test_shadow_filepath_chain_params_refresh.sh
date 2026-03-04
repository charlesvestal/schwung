#!/usr/bin/env bash
set -euo pipefail

shadow_file="src/shadow/shadow_ui.js"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -F -q "function refreshHierarchyChainParams()" "$shadow_file"; then
  echo "FAIL: missing refreshHierarchyChainParams helper" >&2
  exit 1
fi

if ! rg -F -q "refreshHierarchyChainParams();" "$shadow_file"; then
  echo "FAIL: filepath browser open path does not refresh chain_params metadata" >&2
  exit 1
fi

if ! rg -F -q "const effectiveMeta = getParamMetadata(key) || meta;" "$shadow_file"; then
  echo "FAIL: filepath browser does not prioritize refreshed metadata" >&2
  exit 1
fi

echo "PASS: filepath browser refreshes dynamic chain_params metadata"
exit 0
