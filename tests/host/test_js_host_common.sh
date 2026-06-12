#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# C-1 consolidation: the QuickJS scaffolding and the shared file/store/http
# JS bindings (including validate_path / run_command) were extracted from
# schwung_host.c and shadow_ui.c into src/host/js_host_common.c. Both
# binaries register the shared bindings via js_host_register_common().
# This pins the extraction so the copies don't silently come back and drift.

common="src/host/js_host_common.c"
host="src/schwung_host.c"
shadow="src/shadow/shadow_ui.c"

if [ ! -f "$common" ]; then
  echo "FAIL: $common does not exist" >&2
  exit 1
fi

if ! rg -q 'int validate_path\(const char \*path\)' "$common"; then
  echo "FAIL: $common does not define validate_path" >&2
  exit 1
fi

for f in "$host" "$shadow"; do
  if rg -q 'static int validate_path' "$f"; then
    echo "FAIL: $f still defines a private validate_path copy" >&2
    exit 1
  fi
  if ! rg -q 'js_host_register_common\(ctx\)' "$f"; then
    echo "FAIL: $f does not call js_host_register_common" >&2
    exit 1
  fi
done

echo "PASS: js_host_common extraction intact"
