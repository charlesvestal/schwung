#!/usr/bin/env bash
set -euo pipefail

shadow_file="src/shadow/shadow_ui.js"
docs_file="docs/MODULES.md"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -F -q "filepathBrowserState.livePreviewEnabled = parseMetaBool(effectiveMeta.live_preview);" "$shadow_file"; then
  echo "FAIL: live_preview metadata is not read when opening filepath browser" >&2
  exit 1
fi

if ! rg -F -q "filepathBrowserState.previewOriginalValue = currentVal;" "$shadow_file"; then
  echo "FAIL: filepath browser does not snapshot original value for live preview cancel" >&2
  exit 1
fi

if ! rg -F -q "selected.kind === \"file\"" "$shadow_file"; then
  echo "FAIL: filepath browser live preview is not gated to file items" >&2
  exit 1
fi

if ! rg -F -q "filepathBrowserState.previewCommitted = true;" "$shadow_file"; then
  echo "FAIL: filepath browser does not mark preview as committed on select" >&2
  exit 1
fi

if ! rg -F -q "filepathBrowserState.previewCurrentValue !== filepathBrowserState.previewOriginalValue" "$shadow_file"; then
  echo "FAIL: filepath browser cancel path does not compare preview value against original" >&2
  exit 1
fi

if ! rg -F -q "filepathBrowserState.previewOriginalValue || \"\"" "$shadow_file"; then
  echo "FAIL: filepath browser cancel path does not restore original value" >&2
  exit 1
fi

if ! rg -F -q -- "- \`live_preview\` (optional): When true, moving the file-browser cursor over files temporarily sets the parameter to that file until the user confirms or cancels." "$docs_file"; then
  echo "FAIL: docs/MODULES.md does not document filepath live_preview field" >&2
  exit 1
fi

echo "PASS: filepath browser live preview wiring present"
exit 0
