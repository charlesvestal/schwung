#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# 2026-06 cleanup step 9 (U-2): footer-hint verb soup. The live shadow UI
# mixed "Push:" and "Click:" for the same jog-click gesture and mixed
# action-word capitalization ("Jog: Select" vs "Jog: scroll", "Back: Exit"
# vs "Back: exit"). Canon: verb prefix "Click" for jog-click; action word
# lowercase. FOOTER_VERBS in menu_layout.mjs documents the canon for reuse.
#
# Scope: the live on-device UI (shadow_ui.js). chain/ui.js is dead on device
# and store/ui.js is retired, so their footers don't render — out of scope.

shadow="src/shadow/shadow_ui.js"
layout="src/shared/menu_layout.mjs"

# 1. Canonical verb table is exported for reuse.
if ! rg -q 'export const FOOTER_VERBS' "$layout"; then
  echo "FAIL: menu_layout.mjs does not export FOOTER_VERBS (footer-verb canon)" >&2
  exit 1
fi

# 2. No "Push:" footer literal in the live UI — jog-click is "Click".
if rg -q '"Push: ' "$shadow"; then
  echo "FAIL: shadow_ui.js still uses \"Push:\" footer hints (canon is \"Click:\"):" >&2
  rg -n '"Push: ' "$shadow" >&2
  exit 1
fi

# 3. No footer literal with an UPPERCASE action word (canon is lowercase).
#    Matches "Verb: Capital..."; "Click/Back: return" and "Back: " (empty/
#    dynamic) do not match.
if rg -q '"(Click|Push|Jog|Back|Hold|Turn|Press): [A-Z]' "$shadow"; then
  echo "FAIL: shadow_ui.js has footer hints with capitalized action words (canon is lowercase):" >&2
  rg -n '"(Click|Push|Jog|Back|Hold|Turn|Press): [A-Z]' "$shadow" >&2
  exit 1
fi

echo "PASS: shadow_ui.js footer hints use canonical verb + lowercase action word"
