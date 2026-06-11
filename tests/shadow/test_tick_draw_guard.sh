#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

file="src/shadow/shadow_ui.js"

# The C loop (shadow_ui.c) calls tick() unconditionally every frame. Without
# a guard around the draw switch, one throw in any draw function repeats
# every frame — frozen screen with no recovery. The overtake case already
# guards itself; every other view needs the outer guard with a fallback to
# the slots view.

body=$(awk '/^globalThis.tick = function/,/^};/' "$file")
if [ -z "$body" ]; then
  echo "FAIL: globalThis.tick not found" >&2
  exit 1
fi

if ! grep -q 'tick draw EXCEPTION' <<<"$body"; then
  echo "FAIL: tick() draw switch has no exception guard" >&2
  exit 1
fi

# The recovery path must leave the broken view, or the same draw throws
# again next frame.
recovery=$(sed -n '/tick draw EXCEPTION/,/needsRedraw = true/p' <<<"$body")
if ! grep -q 'VIEWS.SLOTS' <<<"$recovery"; then
  echo "FAIL: tick() draw guard does not fall back to the slots view" >&2
  exit 1
fi

echo "PASS: tick() draw path is guarded with slots-view fallback"
