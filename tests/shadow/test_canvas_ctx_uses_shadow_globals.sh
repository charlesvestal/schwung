#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The canvas ctx handed to a module's canvas script must call the SHADOW
# context's drawing globals — the snake_case ones js_display_register_bindings
# puts on the shadow global object.
#
# It did not. `drawLine` was written as
#     if (display && typeof display.drawLine === "function") { ... }
# which looks like a safe capability probe and is not: `display` is built in
# schwung_host.c, a DIFFERENT JSContext, and does not exist in the shadow one.
# An undeclared identifier THROWS on reference, so the guard could never be
# false — it raised a ReferenceError, the host disabled the overlay for the
# session, and a canvas page drew its chrome around an empty body. A module
# that wanted a horizontal rule had to draw it as a 1px fillRect instead.
#
# Two halves, so the test fails if either moves: the JS must call the bare
# global, and the C must still register it on the shadow context.

shadow="src/shadow/shadow_ui.js"
binder="src/host/js_display.c"

# 1. No canvas ctx method reaches for the host-only `display` object.
#    Comment lines are stripped first — this very file names `display.drawLine`
#    in the warning above, and a test that its own explanation trips is a test
#    nobody can leave in place.
code_hits=$(sed -e 's#//.*##' -e '/^[[:space:]]*[*/]/d' "$shadow" \
            | rg -n '(^|[^_a-zA-Z.])display\.' || true)
if [ -n "$code_hits" ]; then
  echo "FAIL: shadow_ui.js calls the \`display\` object, which exists only in the host JSContext:" >&2
  echo "$code_hits" >&2
  exit 1
fi

# 2. drawLine calls the shadow global.
if ! rg -qU 'drawLine\(x1, y1, x2, y2, value\) \{\n\s*draw_line\(' "$shadow"; then
  echo "FAIL: the canvas ctx drawLine does not call the bare draw_line global" >&2
  rg -n -A3 'drawLine\(x1, y1' "$shadow" >&2
  exit 1
fi

# 3. …and that global is on the shadow context.
if ! rg -q 'js_display_register_bindings' src/shadow/shadow_ui.c; then
  echo "FAIL: shadow_ui.c no longer registers the display bindings" >&2
  exit 1
fi
if ! rg -q '"draw_line",' "$binder"; then
  echo "FAIL: js_display_register_bindings no longer registers draw_line" >&2
  exit 1
fi

echo "PASS: canvas ctx draws through the shadow context's own globals"
