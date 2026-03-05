#!/usr/bin/env bash
set -euo pipefail

shadow_file="src/shadow/shadow_ui.js"
docs_file="docs/MODULES.md"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -F -q "function buildFilepathBrowserHooks(meta, prefix) {" "$shadow_file"; then
  echo "FAIL: filepath browser hooks builder is missing" >&2
  exit 1
fi

if ! rg -F -q "onOpen: normalizeFilepathHookActions(hooksRaw.on_open, prefix)" "$shadow_file"; then
  echo "FAIL: filepath browser does not wire on_open hooks" >&2
  exit 1
fi

if ! rg -F -q "onPreview: normalizeFilepathHookActions(hooksRaw.on_preview, prefix)" "$shadow_file"; then
  echo "FAIL: filepath browser does not wire on_preview hooks" >&2
  exit 1
fi

if ! rg -F -q "onCancel: normalizeFilepathHookActions(hooksRaw.on_cancel, prefix)" "$shadow_file"; then
  echo "FAIL: filepath browser does not wire on_cancel hooks" >&2
  exit 1
fi

if ! rg -F -q "onCommit: normalizeFilepathHookActions(hooksRaw.on_commit, prefix)" "$shadow_file"; then
  echo "FAIL: filepath browser does not wire on_commit hooks" >&2
  exit 1
fi

if ! rg -F -q "applyFilepathHookActions(filepathBrowserState, filepathBrowserState.hooksOnOpen" "$shadow_file"; then
  echo "FAIL: filepath browser does not execute on_open hooks" >&2
  exit 1
fi

if ! rg -F -q "applyFilepathHookActions(state, state.hooksOnCommit" "$shadow_file"; then
  echo "FAIL: filepath browser does not execute on_commit hooks" >&2
  exit 1
fi

if ! rg -F -q "applyFilepathHookActions(state, state.hooksOnCancel" "$shadow_file"; then
  echo "FAIL: filepath browser does not execute on_cancel hooks" >&2
  exit 1
fi

if ! rg -F -q -- '- `browser_hooks` (optional): Event hooks to run additional parameter writes at browser lifecycle points. Supported keys: `on_open`, `on_preview`, `on_cancel`, `on_commit`.' "$docs_file"; then
  echo "FAIL: docs/MODULES.md does not document browser_hooks field" >&2
  exit 1
fi

echo "PASS: filepath browser hooks wiring present"
exit 0
