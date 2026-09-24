#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Sort: Type in the swap picker.
#
# The grouping rule is unit-tested here; the picker wiring in shadow_ui.js is
# pinned below. Load-bearing:
#
#  - a module with NO category is filed under Other, LAST, never dropped. A
#    device whose manager has never fetched the catalog must still list every
#    module it has.
#  - a missing or corrupt catalog cache answers null, and grouping with null
#    is still a complete list.
#  - filter BEFORE sort: a heading over a group the filter emptied is a
#    heading over nothing.
#  - the jog steps OVER headings; the cursor never rests on one.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
const M = await import("./src/shared/module_categories.mjs");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (m) => console.error("ok: " + m);
const eq = (a, b, m) => JSON.stringify(a) === JSON.stringify(b) ? ok(m) : fail(m + " -- got " + JSON.stringify(a) + " want " + JSON.stringify(b));
const shape = (rows) => rows.map(r => r.type === "divider" ? "#" + r.label : r.id);

/* ---- type mapping --------------------------------------------------- */
eq(M.taxonomyTypeFor("synth"), "sound_generator", "synth -> sound_generator");
eq(M.taxonomyTypeFor("midiFx"), "midi_fx", "midiFx -> midi_fx");
eq(M.taxonomyTypeFor("midi_fx2"), "midi_fx", "midi_fx2 -> midi_fx");
eq(M.taxonomyTypeFor("fx3"), "audio_fx", "fx3 -> audio_fx");

/* ---- loading -------------------------------------------------------- */
eq(M.loadCatalogCategories(() => null), null, "missing cache -> null");
eq(M.loadCatalogCategories(() => "{nope"), null, "corrupt cache -> null");
eq(M.loadCatalogCategories(() => { throw new Error("x"); }), null, "throwing read -> null");

/* The REAL catalog: every sound generator must resolve, so a taxonomy that
   loses a label or a module that loses its slug shows up here, not on a device. */
const real = M.loadCatalogCategories((p) => readFileSync(p, "utf8"), "module-catalog.json");
if (!real) fail("the shipped catalog does not load");
else {
  const synthOrder = real.order.sound_generator || [];
  synthOrder.length > 0 ? ok("taxonomy carries sound_generator labels") : fail("no sound_generator taxonomy");
  const cat = JSON.parse(readFileSync("module-catalog.json", "utf8"));
  for (const m of cat.modules.filter(m => m.component_type === "sound_generator")) {
    if (!synthOrder.find(s => s.id === real.byId[m.id])) fail(m.id + " has no labelled category");
  }
  const rows = ["dr32", "obxd", "sf2", "zzz-unknown"].map(id => ({ id, name: id }));
  const g = M.groupRowsByCategory(rows, "sound_generator", real);
  eq(shape(g), ["#Polysynth", "obxd", "#Sampler & Rompler", "sf2", "#Drum Machine", "dr32", "#Other", "zzz-unknown"],
     "real catalog: taxonomy order, Other last");
}

/* ---- grouping ------------------------------------------------------- */
const cat = { byId: { a: "poly", b: "drums", c: "poly", p: "drums" },
              order: { sound_generator: [{ id: "poly", label: "Poly" }, { id: "drums", label: "Drums" }] } };
const rows = [{ id: "a" }, { id: "b" }, { id: "c" }, { id: "x" }];
eq(shape(M.groupRowsByCategory(rows, "sound_generator", cat)),
   ["#Poly", "a", "c", "#Drums", "b", "#Other", "x"], "groups in taxonomy order, input order kept within");
eq(shape(M.groupRowsByCategory(rows, "sound_generator", null)),
   ["#Other", "a", "b", "c", "x"], "no catalog: one Other group, nothing dropped");
eq(shape(M.groupRowsByCategory([{ id: "x", subcategory: "drums" }], "sound_generator", cat)),
   ["#Drums", "x"], "module.json subcategory wins over the catalog");
eq(shape(M.groupRowsByCategory([{ id: "p-pack1", parentId: "p" }], "sound_generator", cat)),
   ["#Drums", "p-pack1"], "a pack files under its parent module");
eq(shape(M.groupRowsByCategory([{ id: "y", subcategory: "mono-bass" }], "sound_generator", cat)),
   ["#Mono Bass", "y"], "an unlabelled slug is humanised, not dropped");
eq(shape(M.groupRowsByCategory([], "sound_generator", cat)), [], "no rows, no headings");

/* ---- wiring --------------------------------------------------------- */
const ui = readFileSync("src/shadow/shadow_ui.js", "utf8");
const entry = ui.slice(ui.indexOf("function enterComponentSelect("));
const iFilter = entry.indexOf("pickerApplyFilter(availableModules");
const iSort = entry.indexOf("pickerApplySort(availableModules");
const iMoves = entry.indexOf("...moveEntries");
(iFilter > 0 && iSort > iFilter && iMoves > iSort) ? ok("filter, then sort, then the Move rows")
  : fail("enterComponentSelect must filter, then sort, then splice Move rows (" + [iFilter, iSort, iMoves] + ")");
/\.isCategoryHeader\(availableModules\[next\]\)/.test(ui) ? ok("the jog steps over headings")
  : fail("COMPONENT_SELECT jog no longer skips category headings");
const first = ui.slice(ui.indexOf("function pickerFirstSelectableIndex("), ui.indexOf("function enterComponentSelect("));
/type === "divider"/.test(first) ? ok("the cursor never opens on a heading") : fail("pickerFirstSelectableIndex can land on a heading");

/* The chrome must hand dividers to drawMenuList whole. */
const chrome = readFileSync("src/shared/chain_editor_chrome.mjs", "utf8");
(chrome.match(/type === "divider"/g) || []).length >= 2 ? ok("chrome passes dividers through both maps")
  : fail("drawChainPicker/drawListScreen flatten divider rows into plain rows");

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.error("all module-category checks passed");
'
