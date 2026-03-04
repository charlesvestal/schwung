#!/usr/bin/env bash
set -euo pipefail

shadow_file="src/shadow/shadow_ui.js"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -F -q 'let filepathBrowserSuspendParamKey = "";' "$shadow_file"; then
  echo "FAIL: filepath browser missing suspend param key state" >&2
  exit 1
fi

if ! rg -F -q 'let filepathBrowserSuspendPrevValue = "";' "$shadow_file"; then
  echo "FAIL: filepath browser missing suspend previous value state" >&2
  exit 1
fi

if ! rg -F -q 'const maybeSuspendKey = `${prefix}:ui_auto_select_pad`;' "$shadow_file"; then
  echo "FAIL: filepath browser does not target ui_auto_select_pad for temporary suspend" >&2
  exit 1
fi

if ! rg -F -q 'if (setSlotParam(hierEditorSlot, maybeSuspendKey, "off")) {' "$shadow_file"; then
  echo "FAIL: filepath browser does not disable ui_auto_select_pad on open" >&2
  exit 1
fi

if ! rg -F -q 'setSlotParam(hierEditorSlot, filepathBrowserSuspendParamKey, filepathBrowserSuspendPrevValue);' "$shadow_file"; then
  echo "FAIL: filepath browser does not restore ui_auto_select_pad on close" >&2
  exit 1
fi

echo "PASS: filepath browser temporarily suspends ui_auto_select_pad"
exit 0
