#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

file="src/shadow/shadow_ui.js"

# Saving a preset must never fail silently: when buildSlotPatchJson returns
# null the user pressed Save and nothing happened (same family as the
# autosave silent-bail bug fixed in c265ee1b — the interactive path was
# missed).

save_body=$(awk '/^function doSavePreset\(/,/^}/' "$file")
if [ -z "$save_body" ]; then
  echo "FAIL: doSavePreset missing" >&2
  exit 1
fi
if grep -q 'TODO: show error message' <<<"$save_body"; then
  echo "FAIL: doSavePreset still bails silently (literal TODO present)" >&2
  exit 1
fi
if ! grep -Eq 'showWarning\(|warningActive = true' <<<"$save_body"; then
  echo "FAIL: doSavePreset does not surface an error to the user on failed save" >&2
  exit 1
fi

# Autosave must not record a slot as saved when the write failed, or the
# retry is suppressed until the next module change.
auto_body=$(awk '/^function autosaveAllSlots\(/,/^}/' "$file")
if ! grep -Eq 'if \(host_write_file\(|= host_write_file\(' <<<"$auto_body"; then
  echo "FAIL: autosaveAllSlots ignores host_write_file result" >&2
  exit 1
fi

echo "PASS: preset save failures are surfaced; autosave checks writes"
