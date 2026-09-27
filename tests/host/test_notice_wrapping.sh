#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A NOTICE IS A SENTENCE, and the box has to hold one.
#
# It used to be one line, sized to its text or the frame, and it CLIPPED in
# silence past that: "Step automation cleared" is ~138px in the 5x7 font
# against a 128px screen, so the last word simply was not drawn. Same class as
# the footer dropping a hint pair -- a message with no way to say it did not
# fit. And SHOUTING every message made each one read as an error; "step
# cleared" is a receipt, not an alarm.

fail() { echo "FAIL: $1"; exit 1; }
command -v node >/dev/null || { echo "SKIP: node not available"; exit 0; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# No toast SHOUTS. A word may be capitalised (a module name, an abbreviation);
# a whole multi-word message in caps is the thing being removed.
node -e '
const fs = require("fs");
const src = fs.readFileSync("src/shared/param_pages/page_controller.mjs", "utf8");
const bad = [];
const bad0 = [];

const re = /notice\(\s*"([^"]{4,})"/g;
let m;
while ((m = re.exec(src))) {
    const t = m[1];
    const words = t.split(/[\s:,]+/).filter((w) => /[A-Za-z]{2,}/.test(w));
    const shouty = words.filter((w) => w === w.toUpperCase() && /[A-Z]{2,}/.test(w));
    if (words.length >= 2 && shouty.length === words.length) bad.push(t);
}
if (bad.length) { for (const b of bad) console.log("FAIL: all-caps notice: " + b); process.exit(1); }
console.log("ok  no notice shouts");
' || fail "an all-caps notice came back"

cat > "$tmp/t.mjs" <<EOF
const REPO = "$PWD";
EOF
echo "PASS: notices do not shout"
