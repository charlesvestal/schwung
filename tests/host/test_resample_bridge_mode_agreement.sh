#!/usr/bin/env bash
# The C and JS readers of `resample_bridge_mode` must agree.
#
# One config key, two parsers: resample_bridge_mode_from_text_pure() in
# src/host/resample_bridge_mode.h, and parseResampleBridgeMode() in
# src/shadow/shadow_ui.js. They disagreed about the legacy value `1` — JS
# migrated it to 2, C returned the retired mode 1 — and C runs first, at shim
# init. Nothing anywhere said they had to match, which is exactly how one fact
# with two consumers goes wrong.
#
# This runs BOTH over one table rather than restating either's expectations.
set -uo pipefail
cd "$(dirname "$0")/../.."

fail() { echo "FAIL: $*"; exit 1; }

BIN=build/tests/host/test_resample_bridge_mode
JS=src/shadow/shadow_ui.js
[ -f "$JS" ] || fail "missing $JS"

command -v node >/dev/null 2>&1 || { echo "SKIP: node not available"; exit 0; }

mkdir -p build/tests/host
cc -Isrc/host -o "$BIN" tests/host/test_resample_bridge_mode.c || fail "C build failed"
"$BIN" >/dev/null || fail "C self-check failed (run $BIN for detail)"

# Lift the JS function out verbatim. Anchored on its own declaration and closed
# on the first line that is a bare "}" at column 0, which is how every function
# in that file ends.
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
awk '/^function parseResampleBridgeMode\(raw\) \{/,/^\}/' "$JS" > "$TMP/fn.js"
[ -s "$TMP/fn.js" ] || fail "could not extract parseResampleBridgeMode from $JS"
grep -q '^}' "$TMP/fn.js" || fail "extracted JS function is not closed — the anchor moved"
printf '\nexport { parseResampleBridgeMode };\n' >> "$TMP/fn.js"

cat > "$TMP/run.mjs" <<'EOF'
import { parseResampleBridgeMode } from './fn.js';
process.stdout.write(String(parseResampleBridgeMode(process.argv[2] ?? null)));
EOF

# The table. Every spelling either side has ever accepted, plus the legacy one.
INPUTS=("0" "1" "2" "3" "off" "Off" "mix" "Mix" "MIX" "overwrite" "replace"
        "REPLACE" " 2 " "" "native" "garbage")

mismatch=0
for in in "${INPUTS[@]}"; do
    c=$("$BIN" "$in")
    j=$(node "$TMP/run.mjs" "$in")
    if [ "$c" != "$j" ]; then
        echo "MISMATCH  input=\"$in\"  C=$c  JS=$j"
        mismatch=1
    fi
    if [ "$c" = "1" ] || [ "$j" = "1" ]; then
        echo "RETIRED   input=\"$in\" produced mode 1 (C=$c JS=$j)"
        mismatch=1
    fi
done

[ "$mismatch" -eq 0 ] || fail "the two resample_bridge_mode parsers disagree"

echo "PASS: C and JS agree on resample_bridge_mode for ${#INPUTS[@]} inputs, and neither can produce the retired mode 1"
