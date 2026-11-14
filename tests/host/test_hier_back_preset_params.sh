#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# BACK FROM A CATEGORY RETURNS TO THE CATEGORY LIST, NOT THE PRESET LIST.
#
# OB-Xd, Dexed, Hera, Braids, Osirus and nine more put their categories (Osc 1,
# Filter, LFO ...) as navigation params on a PRESET root. In the list editor
# (screen reader on, or Param View = List) those rows only show in preset EDIT
# mode, and Back from Osc 1 reloaded root -- which loadHierarchyLevel resets to
# the BROWSER. The user heard "Parameters" and was in the preset list, with no
# way to reach Osc 2 except jog-click back into edit mode and scroll from the
# top. Reported from the field, "across schwung versions".
#
# Part 1 runs the helper against every captured fleet contract; part 2 pins
# that the Back case actually consults it.

node --input-type=module -e '
import fs from "fs";
import { hierBackParent, hierBackRow, hierBackReopensPresetEdit }
    from "./src/shared/hier_back.mjs";

let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };
const ok = (m) => console.log("  ok  " + m);

/* ---- 1. every fleet module with categories on a preset level ------------ */
const d = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
const mods = Array.isArray(d.modules) ? d.modules : (Array.isArray(d) ? d : Object.values(d.modules || d));
let checked = 0;
const ids = new Set();
for (const m of mods) {
    let h = m.ui_hierarchy;
    if (typeof h === "string") { try { h = JSON.parse(h); } catch { continue; } }
    if (!h || !h.levels) continue;
    for (const [name, lv] of Object.entries(h.levels)) {
        if (!lv || !lv.list_param || !lv.count_param || lv.children) continue;
        const params = Array.isArray(lv.params) ? lv.params : [];
        params.forEach((p, i) => {
            if (!p || typeof p !== "object" || !p.level || !h.levels[p.level]) return;
            checked++; ids.add(m.id);
            const r = hierBackParent(h.levels, p.level);
            if (r.parent !== name || !r.viaNavParam)
                fail(`${m.id}: Back from ${p.level} -> ${r.parent} (nav=${r.viaNavParam}), want ${name}`);
            else if (!hierBackReopensPresetEdit(h.levels[r.parent], r.viaNavParam))
                fail(`${m.id}: Back from ${p.level} reopens the preset BROWSER`);
            else if (hierBackRow(params, p.level) !== i)
                fail(`${m.id}: Back from ${p.level} lands on row ${hierBackRow(params, p.level)}, want ${i}`);
        });
    }
}
if (ids.size < 10) fail(`only ${ids.size} fleet modules exercised -- fixture shape changed?`);
else ok(`${checked} categories across ${ids.size} modules return to their row in edit mode`);

/* A level drilled into through `children` is NOT a nav param: Back from it
 * returns to the browser it came from (Surge: root -> main). */
{
    const levels = { root: { list_param: "preset", count_param: "n", children: "main" },
                     main: { params: [{ level: "osc1" }] }, osc1: { params: ["a"] } };
    const r = hierBackParent(levels, "main");
    if (r.parent !== "root" || r.viaNavParam || hierBackReopensPresetEdit(levels.root, r.viaNavParam))
        fail("children-drilled level would reopen preset edit mode");
    else ok("children-drilled level returns to the browser");
    const r2 = hierBackParent(levels, "osc1");
    if (r2.parent !== "main" || hierBackReopensPresetEdit(levels.main, r2.viaNavParam))
        fail("plain-level parent misreported");
    else ok("plain-level parent unchanged");
}

/* ---- 2. the Back case uses it ------------------------------------------- */
const ui = fs.readFileSync("src/shadow/shadow_ui.js", "utf8")
    .replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
const at = ui.indexOf("case VIEWS.HIERARCHY_EDITOR: {", ui.indexOf("\nfunction handleBack("));
const body = ui.slice(at, ui.indexOf("case VIEWS.KNOB_EDITOR", at));
if (at < 0 || !body) fail("handleBack HIERARCHY_EDITOR case not found");
else {
    const a = body.indexOf("hierBackParent(");
    const b = body.indexOf("loadHierarchyLevel()", a);
    const c = body.indexOf("hierBackReopensPresetEdit(", b);
    const e = body.indexOf("hierEditorPresetEditMode = true", c);
    if (a < 0 || b < 0 || c < 0 || e < 0)
        fail("Back no longer reopens preset edit mode AFTER loadHierarchyLevel (which resets it)");
    else ok("Back restores preset edit mode after reloading the parent");
    if (body.indexOf("hierBackRow(") < 0) fail("Back no longer restores the cursor row");
    else ok("Back restores the cursor row");
}

if (failures) { console.log(`${failures} failure(s)`); process.exit(1); }
'
