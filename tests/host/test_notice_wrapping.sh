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
cat >> "$tmp/t.mjs" <<'EOF'
import fs from "fs";
const { createController, LAYOUT_MOVY } = await import(REPO + "/src/shared/param_pages/page_controller.mjs");
const { createFramebuffer, drawContext } = await import(REPO + "/tools/param-pages/harness.mjs");
const mods = JSON.parse(fs.readFileSync(REPO + "/tests/fixtures/module-contracts.json", "utf8")).modules;
const m = mods.find((x) => x.id === "9w9");
const str = (v) => (typeof v === "string" ? v : JSON.stringify(v));

let held = 4;
const writes = [];
const ctrl = createController({
    getParam: (k) => k === "synth:ui_hierarchy" ? str(m.ui_hierarchy)
                  : k === "synth:chain_params" ? str(m.chain_params)
                  : k === "lanes:step_locks" ? "4369 273"
                  : k.endsWith(":modulated") ? "0"
                  : k.endsWith(":held") ? "" : "74",
    setParam: (k, v) => { writes.push(k); return true; },
    announce: () => {}, heldStep: () => held,
});
ctrl.load({ slot: 0, component: "synth", prefix: "synth" });
ctrl.setLayout(LAYOUT_MOVY);
ctrl.dismissHint && ctrl.dismissHint();
for (let i = 0; i < 12; i++) ctrl.tick();

const shot = () => {
    const fb = createFramebuffer();
    ctrl.render(drawContext(fb), { title: "9W9", footer: [["JOG", "PAGE"]] });
    ctrl.renderOverlays(drawContext(fb), { clearScreen: () => {} });
    return fb;
};
const rowsWithInk = (fb) => {
    const out = [];
    for (let y = 0; y < 64; y++) {
        let n = 0;
        for (let x = 0; x < 128; x++) n += fb.pixels[y * 128 + x] ? 1 : 0;
        out.push(n);
    }
    return out;
};
const bad = [];
const bad0 = [];


/* THE MAP IS ON SCREEN FIRST. That is the real sequence -- you hold a step,
 * the strip tells you which steps carry locks, and then you clear one. A test
 * that clears before the map has ever been fetched proves nothing about
 * invalidating it. */
shot();
const fetchedBefore = writes.filter((k) => k === "lanes:step_locks_query").length;
if (fetchedBefore !== 1) bad0.push("the map was not fetched by the first frame (" + fetchedBefore + ")");

/* The clear gesture's own message, which is the one that did not fit. */
ctrl.onEditCc(119, true);
ctrl.onEditCc(119, false);
const fb = shot();
/* The box's own top and bottom rules: a long horizontal run, which the grid's
 * own rows never produce apart from the bank bar (full width, row 7). Sized
 * from the TEXT, so the threshold is well under the screen width. */
const rows = rowsWithInk(fb);
const borders = [];
for (let y = 8; y < 64; y++) if (rows[y] > 60) borders.push(y);
if (borders.length < 2) bad.push("no notice box was drawn");
else if (borders[borders.length - 1] - borders[0] < 14)
    bad.push("the notice stayed one line high (" + (borders[borders.length-1] - borders[0]) + "px) -- it clipped instead of wrapping");

/* Nothing may be drawn OUTSIDE the frame: a wrapped box still has to fit. */
if (borders[0] < 0 || borders[borders.length - 1] > 63)
    bad.push("the notice box left the screen");

/* AND THE MAP IS REFETCHED, because a clear happens inside the hold that is
 * displaying it -- the strip otherwise keeps a mark for automation that was
 * just deleted, exactly when the user is looking for confirmation. The ORDER
 * is the assertion: a query AFTER the clear, not merely one somewhere. */
const cleared = writes.lastIndexOf("lanes:clear_step");
const queried = writes.lastIndexOf("lanes:step_locks_query");
if (cleared < 0) bad.push("the clear never reached the chain");
else if (!(queried > cleared))
    bad.push("the lock map was not refetched after the clear (clear@" + cleared +
             ", query@" + queried + ")");

for (const b of bad0) bad.unshift(b);
if (bad.length) { for (const b of bad) console.log("FAIL: " + b); process.exit(1); }
console.log("ok  a long notice wraps inside the frame, and a clear refreshes the map");
EOF
node "$tmp/t.mjs" || fail "notice wrapping"
echo "PASS: notices wrap, and do not shout"
