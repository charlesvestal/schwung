#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

file="src/schwung_shim.c"

# features.json is parsed by strstr over the raw file contents. A fixed
# 512-byte read truncates real-world configs (11+ keys), silently reverting
# any key past the cut (e.g. ext_midi_remap_enabled, shadow_ui_trigger) to
# its default. The loader must read the whole file.

body=$(awk '/^static void load_feature_config\(void\)/,/^}/' "$file")
if [ -z "$body" ]; then
  echo "FAIL: load_feature_config missing" >&2
  exit 1
fi

if grep -q 'char config_buf\[512\]' <<<"$body"; then
  echo "FAIL: load_feature_config still reads into a fixed 512-byte buffer" >&2
  exit 1
fi

# Whole-file read: size the buffer from the file (fseek/ftell or fstat).
if ! grep -Eq 'ftell|fstat' <<<"$body"; then
  echo "FAIL: load_feature_config does not size its buffer from the file" >&2
  exit 1
fi

echo "PASS: load_feature_config reads the whole features.json"
