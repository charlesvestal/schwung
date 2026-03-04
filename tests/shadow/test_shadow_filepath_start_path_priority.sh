#!/usr/bin/env bash
set -euo pipefail

browser_file="src/shared/filepath_browser.mjs"
docs_file="docs/MODULES.md"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -F -q "meta.start_path" "$browser_file"; then
  echo "FAIL: filepath browser does not read optional start_path metadata" >&2
  exit 1
fi

if ! rg -F -q "currentValue && currentValue.length > 0" "$browser_file"; then
  echo "FAIL: filepath browser no longer prioritizes current param value" >&2
  exit 1
fi

if ! rg -F -q "(meta.start_path || root)" "$browser_file"; then
  echo "FAIL: filepath browser does not fall back to start_path before root" >&2
  exit 1
fi

if ! rg -F -q -- "- \`start_path\` (optional): Absolute folder or file path used as the initial location when current value is empty." "$docs_file"; then
  echo "FAIL: docs/MODULES.md does not document filepath start_path field" >&2
  exit 1
fi

echo "PASS: filepath browser start_path priority wiring present"
exit 0
