#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

file="src/shadow/shadow_ui.c"

# Six JS-binding setters read-modify-write features.json through fixed
# 512/1024-byte buffers. Once the config grows past the buffer, ANY toggle
# (display mirror, MIDI indicator, skipback...) rewrites features.json from
# the truncated copy — destroying every key past the cut. All setters must
# go through one whole-file helper.

if ! rg -q 'static void features_json_set\(' "$file"; then
  echo "FAIL: shared features_json_set helper missing" >&2
  exit 1
fi

helper=$(awk '/^static void features_json_set\(/,/^}/' "$file")
if ! grep -Eq 'ftell|fstat' <<<"$helper"; then
  echo "FAIL: features_json_set does not size its buffer from the file" >&2
  exit 1
fi

for fn in js_display_mirror_set js_set_pages_set js_midi_indicator_set \
          js_shadow_ui_trigger_set js_skipback_shortcut_set js_skipback_seconds_set; do
  body=$(awk "/^static JSValue ${fn}\(/,/^}/" "$file")
  if [ -z "$body" ]; then
    echo "FAIL: ${fn} missing" >&2
    exit 1
  fi
  if ! grep -q 'features_json_set(' <<<"$body"; then
    echo "FAIL: ${fn} does not use features_json_set" >&2
    exit 1
  fi
  if grep -Eq 'char (buf|newbuf)\[(512|1024)\]' <<<"$body"; then
    echo "FAIL: ${fn} still has a fixed-buffer features.json RMW" >&2
    exit 1
  fi
done

echo "PASS: all features.json setters use the whole-file helper"
