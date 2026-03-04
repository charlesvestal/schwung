#!/usr/bin/env bash
set -euo pipefail

shadow_file="src/shadow/shadow_ui.js"
docs_file="docs/MODULES.md"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -F -q "parseMetaBool(meta && meta.suspend_auto_select)" "$shadow_file"; then
  echo "FAIL: filepath browser missing suspend_auto_select backward-compatible alias" >&2
  exit 1
fi

if ! rg -F -q 'key: `${prefix}:ui_auto_select_pad`' "$shadow_file"; then
  echo "FAIL: filepath browser suspend alias does not target ui_auto_select_pad" >&2
  exit 1
fi

if ! rg -F -q "value: \"off\"" "$shadow_file"; then
  echo "FAIL: filepath browser suspend alias does not force off value" >&2
  exit 1
fi

if ! rg -F -q "restore: true" "$shadow_file"; then
  echo "FAIL: filepath browser suspend alias does not request restoration" >&2
  exit 1
fi

if ! rg -F -q "applyFilepathHookActions(filepathBrowserState, filepathBrowserState.hooksOnOpen" "$shadow_file"; then
  echo "FAIL: filepath browser does not apply open hooks (including suspend alias)" >&2
  exit 1
fi

if ! rg -F -q "restoreFilepathHookActions(state);" "$shadow_file"; then
  echo "FAIL: filepath browser does not restore hook-managed params on close" >&2
  exit 1
fi

if ! rg -F -q -- '- `suspend_auto_select` (optional): When true, Shadow temporarily sets `<component>:ui_auto_select_pad` to `"off"` while the browser is open, then restores it on close.' "$docs_file"; then
  echo "FAIL: docs/MODULES.md does not document filepath suspend_auto_select field" >&2
  exit 1
fi

echo "PASS: filepath browser temporarily suspends ui_auto_select_pad"
exit 0
