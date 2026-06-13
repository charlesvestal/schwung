#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# 2026-06 cleanup step 9 (U-3): file-browser hand-rolled its own list
# renderer (drawList: manual print + scrollbar, no marquee). Migrated onto
# the shared drawMenuList for long-filename marquee scrolling + scroll-arrow
# indicators that match the rest of the UI.
#
# file-browser ALREADY emits its own (richer, contextual) screen-reader
# announcements, and drawMenuList announces the selected item on draw via
# shared singleton state — so the migration MUST pass announce:false to
# avoid double-announcing every selection move. That required an additive
# `announce` option on drawMenuList (compat-safe for external importers).

layout="src/shared/menu_layout.mjs"
fb="src/modules/tools/file-browser/ui.js"

# 1. drawMenuList honours an `announce` opt-out (default on).
if ! rg -q 'announce = true' "$layout"; then
  echo "FAIL: drawMenuList has no announce option (needed so file-browser can opt out)" >&2
  exit 1
fi
# The internal announce must be guarded by the flag.
if ! rg -q 'if \(announce &&' "$layout"; then
  echo "FAIL: drawMenuList announce block is not guarded by the announce flag" >&2
  exit 1
fi

# 2. file-browser imports and delegates to drawMenuList.
if ! rg -q 'drawMenuList' "$fb"; then
  echo "FAIL: file-browser does not use the shared drawMenuList" >&2
  exit 1
fi
# 3. It opts out of drawMenuList's announcements (keeps its own).
if ! rg -q 'announce: false' "$fb"; then
  echo "FAIL: file-browser does not pass announce:false to drawMenuList (double-announce risk)" >&2
  exit 1
fi
# 4. The hand-rolled per-row print/scrollbar loop is gone from drawList.
if rg -q 'fill_rect\(SCREEN_W - 2, barY' "$fb"; then
  echo "FAIL: file-browser still hand-draws its own scrollbar (drawList not migrated)" >&2
  exit 1
fi

echo "PASS: file-browser list renders via shared drawMenuList (announce:false)"
