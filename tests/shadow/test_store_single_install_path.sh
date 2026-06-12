#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

f="src/shadow/shadow_ui.js"

# KISS: schwung-manager (move.local:7700) is the single install/update
# path. On-device keeps detection (Check Updates summary) and pointer
# screens only. The interactive flows (store browser, UPDATE_PROMPT with
# per-module install + Update All, staged core updates) were already
# unreachable — every entry point was redirected to pointer screens — and
# are deleted. This test pins the retirement.

for dead in 'sharedInstallModule' 'processAllUpdates' 'performCoreUpdate' \
            'STORE_PICKER_CATEGORIES' 'STORE_PICKER_LIST' 'STORE_PICKER_DETAIL' \
            'UPDATE_PROMPT' 'UPDATE_RESTART' 'buildStoreCategoryItems' \
            'getHostUpdateModule'; do
  if rg -q "$dead" "$f"; then
    echo "FAIL: retired install-path symbol still present: $dead" >&2
    exit 1
  fi
done

# The live remainder: detection + pointer screens.
for live in 'showUpdatesAvailableScreen' 'showModuleStorePointer' \
            'checkForUpdatesInBackground' 'STORE_PICKER_RESULT' \
            'move.local:7700'; do
  if ! rg -q "$live" "$f"; then
    echo "FAIL: live detection/pointer surface missing: $live" >&2
    exit 1
  fi
done

# The dead browser draws must be gone from the store view module too.
for dead in 'drawStorePickerCategories' 'drawStorePickerList' 'drawStorePickerDetail'; do
  if rg -q "$dead" src/shadow/shadow_ui_store.mjs "$f"; then
    echo "FAIL: dead store-browser draw still present: $dead" >&2
    exit 1
  fi
done

echo "PASS: schwung-manager is the single install/update path"
