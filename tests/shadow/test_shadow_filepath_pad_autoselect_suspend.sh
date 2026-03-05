#!/usr/bin/env bash
set -euo pipefail

shadow_file="src/shadow/shadow_ui.js"
docs_file="docs/MODULES.md"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if rg -F -q "suspend_auto_select" "$shadow_file"; then
  echo "FAIL: filepath browser still contains suspend_auto_select alias behavior" >&2
  exit 1
fi

if rg -F -q -- '- `suspend_auto_select` (optional):' "$docs_file"; then
  echo "FAIL: docs/MODULES.md still documents suspend_auto_select alias field" >&2
  exit 1
fi

if ! rg -F -q "normalizeFilepathHookActions(hooksRaw.on_open, prefix)" "$shadow_file"; then
  echo "FAIL: filepath browser missing on_open hook wiring" >&2
  exit 1
fi

if ! rg -F -q "applyFilepathHookActions(filepathBrowserState, filepathBrowserState.hooksOnOpen" "$shadow_file"; then
  echo "FAIL: filepath browser does not apply on_open hook actions" >&2
  exit 1
fi

if ! rg -F -q "restoreFilepathHookActions(state);" "$shadow_file"; then
  echo "FAIL: filepath browser does not restore hook-managed params on close" >&2
  exit 1
fi

if ! rg -F -q -- '- For pad samplers, you can suspend auto-pad switching while browsing by adding `{"key":"ui_auto_select_pad","value":"off","restore":true}` to `browser_hooks.on_open`.' "$docs_file"; then
  echo "FAIL: docs/MODULES.md missing browser_hooks-based pad auto-select suspend guidance" >&2
  exit 1
fi

echo "PASS: filepath browser uses browser_hooks for pad auto-select suspension"
exit 0
