#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A MODULE MAY DECLARE MORE THAN ONE CUSTOM WIDGET.
#
# The registry has always been a Map, so nothing about the design limited a
# module to one widget -- the single limit was the shadow_ui call site reading
# one `widgetKind` STRING. A module declaring two kinds got the first one
# registered and the second silently ignored, and an ignored kind is not an
# error: resolveViz leaves those keys to the detector, so the cell draws a
# perfectly reasonable built-in dial. A correct-looking page, no log line, and
# nothing to search for.
#
# What must hold:
#   - the legacy single `widgetKind` keeps working, untouched
#   - `widgetKinds` as an ARRAY registers every name against the one drawCell
#   - `widgetKinds` as an OBJECT gives each kind its own drawer and nominal
#   - anything unusable is REPORTED rather than dropped, because "declared a
#     widget and did not get one" is the thing an author cannot otherwise see
#   - the call site in shadow_ui.js actually uses this, rather than keeping its
#     own copy of the rule
#
# NO APOSTROPHES BELOW THIS LINE inside the node script: it is a single-quoted
# bash string, and one apostrophe ends it early with an error pointing nowhere
# near the real line.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the widget registry tests" >&2
  exit 1
fi

node --input-type=module -e '
import { registerOverlayWidgets, clearWidgets, isWidgetAvailable, getWidget }
  from "./src/shared/param_pages/widget_registry.mjs";
import { readFileSync } from "node:fs";

let fails = 0;
const ok = (cond, what) => { if (!cond) { console.error("FAIL: " + what); fails++; } };

/* ---- the legacy shape is untouched ---- */
clearWidgets();
let r = registerOverlayWidgets({ widgetKind: "custom:solo", drawCell() {} });
ok(r.registered.length === 1 && r.registered[0] === "custom:solo", "legacy widgetKind registers");
ok(isWidgetAvailable("custom:solo"), "legacy kind is available");
ok(r.skipped.length === 0, "legacy kind reports nothing skipped");

/* ---- an array: several names, one drawer ---- */
clearWidgets();
let drawn = [];
r = registerOverlayWidgets({
  widgetKinds: ["custom:face", "custom:mouth"],
  drawCell(ctx, o) { drawn.push(o.group.keys[0]); },
});
ok(r.registered.length === 2, "array form registers both kinds");
ok(isWidgetAvailable("custom:face") && isWidgetAvailable("custom:mouth"),
   "both kinds from the array are available");
/* Both must reach the SAME drawer -- that is the point of the array form. */
getWidget("custom:face").draw({}, { group: { keys: ["a"] } });
getWidget("custom:mouth").draw({}, { group: { keys: ["b"] } });
ok(drawn.join(",") === "a,b", "both array kinds call the one drawCell");

/* ---- an object: a drawer each, and its own nominal ---- */
clearWidgets();
let hitX = 0;
r = registerOverlayWidgets({
  widgetNominal: { w: 1, h: 1 },
  widgetKinds: {
    "custom:x": () => { hitX++; },
    "custom:y": { draw() {}, nominal: { w: 17, h: 15 } },
  },
});
ok(r.registered.length === 2, "object form registers both kinds");
getWidget("custom:x").draw({}, {});
ok(hitX === 1, "a bare function entry is used as the drawer");
ok(getWidget("custom:y").nominal.w === 17, "an object entry carries its own nominal");
ok(getWidget("custom:x").nominal.w === 1, "an entry without a nominal falls back to widgetNominal");

/* ---- `this` must still be the overlay ---- */
clearWidgets();
const ov = {
  tag: "me",
  seen: null,
  widgetKinds: { "custom:t": function () { this.seen = this.tag; } },
};
registerOverlayWidgets(ov);
getWidget("custom:t").draw({}, {});
ok(ov.seen === "me", "a drawer is bound to the overlay, so this.helper works");

/* ---- unusable declarations are reported, not dropped ---- */
clearWidgets();
r = registerOverlayWidgets({ widgetKinds: ["plain", "custom:nodrawer"] });
ok(r.registered.length === 0, "nothing usable registers");
ok(r.skipped.length === 2, "both bad declarations are reported");
ok(r.skipped.some((x) => x.kind === "plain" && /custom/.test(x.why)),
   "a kind missing the custom: prefix says so");
ok(r.skipped.some((x) => x.kind === "custom:nodrawer" && /drawCell/.test(x.why)),
   "a kind with no drawer says so");

clearWidgets();
r = registerOverlayWidgets({ widgetKinds: 42 });
ok(r.skipped.length === 1, "a widgetKinds that is neither array nor object is reported");

/* ---- junk must not throw ---- */
clearWidgets();
for (const junk of [null, undefined, 7, "x", {}, { widgetKind: 5 }]) {
  const out = registerOverlayWidgets(junk);
  ok(out && Array.isArray(out.registered), "junk overlay returns a result rather than throwing");
}

/* ---- the call site USES it ---- */
const ui = readFileSync("./src/shadow/shadow_ui.js", "utf8");
ok(/registerOverlayWidgets\(/.test(ui),
   "shadow_ui.js calls registerOverlayWidgets");
ok(!/registerWidget\(ov\.widgetKind/.test(ui),
   "shadow_ui.js no longer hand-rolls single-kind registration");

if (fails) { console.error(fails + " failure(s)"); process.exit(1); }
console.log("PASS: a module may declare several custom widgets, and an unusable one is reported");
'
