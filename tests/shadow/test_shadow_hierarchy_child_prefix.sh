#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
FILE="${REPO_ROOT}/src/shadow/shadow_ui.js"

rg -n "child_prefix" "$FILE" >/dev/null
rg -n "child_count" "$FILE" >/dev/null
rg -n "hierEditorChildIndex" "$FILE" >/dev/null

echo "PASS: hierarchy child prefix support present"
