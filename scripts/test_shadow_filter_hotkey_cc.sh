#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

python3 - <<'PY'
path = "src/move_anything_shim.c"
with open(path, "r", encoding="utf-8") as f:
    lines = f.readlines()

start = None
for idx, line in enumerate(lines):
    if "static void shadow_filter_move_input" in line:
        start = idx
        break

if start is None:
    print("FAIL: shadow_filter_move_input not found")
    raise SystemExit(1)

brace_count = 0
body_lines = []
found_open = False
for line in lines[start:]:
    if not found_open:
        if "{" in line:
            found_open = True
            brace_count += line.count("{") - line.count("}")
        body_lines.append(line)
        continue
    brace_count += line.count("{") - line.count("}")
    body_lines.append(line)
    if brace_count == 0:
        break

body = "".join(body_lines)
pos_hotkey = body.find("shadow_is_hotkey_event")
pos_cc = body.find("if (type == 0xB0)")

if pos_hotkey == -1 or pos_cc == -1:
    print("FAIL: missing hotkey or CC filter in shadow_filter_move_input")
    raise SystemExit(1)

if pos_hotkey < pos_cc:
    print("PASS: hotkey check occurs before CC filter")
    raise SystemExit(0)

print("FAIL: hotkey check occurs after CC filter (shift CCs will be dropped)")
raise SystemExit(1)
PY
