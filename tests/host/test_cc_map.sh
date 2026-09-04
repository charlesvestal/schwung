#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE CC MAP: what the list is built from, and what the walk does with it.
#
# Three things here, and each one shipped broken at least once today.
#
# 1. PAGE NAMES. A module declares four parameters called "Gain" and the list is
#    unreadable without the page each sits on. The page name is derived from the
#    module's own ui_pages/ui_hierarchy, so this must keep working for any module
#    without knowing its schema -- which is exactly the property a test protects
#    and code review does not.
#
# 2. MENU ROWS THAT NAVIGATE. page_plan documents { label, level } and
#    mapMenuEntries has always carried it, but the walk never followed it, so a
#    row drew correctly, moved the cursor correctly, and did nothing when
#    clicked. Nothing failed; the destination simply did not exist.
#
# 3. HIDDEN PAGES. A page opened from a menu row is not in the jog order. It was,
#    and the page rule counted it, so the header drew tabs pointing at pages the
#    jog could never reach.
#
# NO APOSTROPHES BELOW THIS LINE inside the node script: it is a single-quoted
# bash string, and one apostrophe ends it early with an error pointing nowhere
# near the real line.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the cc map tests" >&2
  exit 1
fi

node --input-type=module -e '
import { slotGridHierarchy, allSlotGridParams, ccMapMenu, ccComponents, realKeyFor }
  from "./src/shadow/shadow_ui_slot_grid.mjs";
import { planPages } from "./src/shared/param_pages/page_plan.mjs";
import { step } from "./src/shared/param_pages/page_nav.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* Two components. The EQ has the four-Gain problem on purpose. */
const MAP = [
  "12|synth|bd_tune|BD Tune|Bass Drum",
  "-1|synth|bd_decay|BD Decay|Bass Drum",
  "-1|fx1|g1|Gain|Band 1",
  "-1|fx1|g2|Gain|Band 2",
  "-1|fx1|hpf|HPF|Filters",
  "-1|fx1|b1g|Band 1 Gain|Band 1",
].join(";") + ";";
const COMPS = "synth|9w9|1|1;fx1|4k-eq|1|0;";

/* ---- 1. page names ---- */
const rows = ccMapMenu(MAP);
const label = (i) => rows[i].label;
ok(label(2) === "Band 1 Gain", "page name disambiguates the first Gain");
ok(label(3) === "Band 2 Gain", "page name disambiguates the second Gain");
ok(label(4) === "Filters HPF", "page name prefixes a non-colliding row too");
ok(label(5) === "Band 1 Gain",
   "a name that already starts with its page is not doubled");
ok(rows[0].index === 0 && rows[5].index === 5,
   "rows carry their index, which is how the card addresses them");

/* ---- unassigned ---- */
ok(String(rows[1].cc) === "-1", "an unassigned row reports -1, it is still listed");

/* ---- components ---- */
const comps = ccComponents(COMPS);
ok(comps.length === 2 && comps[0].module === "9w9", "components parse");
ok(comps[0].on === true && comps[1].on === false,
   "a component whose gate is closed says so");

/* ---- key mapping: no colons, or bare() eats half the key ---- */
ok(realKeyFor("cc_synth") === "cc_control:synth", "component gate maps to its device key");
ok(realKeyFor("cc3") === "cc_idx:3", "a CC row maps to its index");

/* ---- 2 + 3. the walk ---- */
const hierarchy = slotGridHierarchy(false, MAP, COMPS);
const chainParams = allSlotGridParams(MAP);
const planned = planPages({ hierarchy, chainParams });
const pages = planned.pages;
const levelOf = (n) => pages.findIndex((p) => p && p.level === n);

ok(levelOf("ccmap") >= 0, "the CC Map index is planned");
ok(levelOf("ccmap_synth") >= 0,
   "a level reached ONLY from a menu row still gets a page");
ok(levelOf("ccmap_fx1") >= 0, "...for every component");

ok(pages[levelOf("ccmap_synth")].hidden === true, "component pages are hidden");
ok(pages[levelOf("ccmap")].hidden !== true, "the index itself is not");

/* Jogging forward from the index must not walk into a hidden page. */
const idx = levelOf("ccmap");
ok(step(pages, idx, 1) === idx,
   "the jog does not walk into a hidden page");
ok(step(pages, idx, -1) === idx - 1,
   "...and still steps backwards normally");

/* The index rows navigate rather than act. */
const menu = hierarchy.levels.ccmap.menu;
ok(menu.length === 2 && menu[0].level === "ccmap_synth",
   "index rows carry a level, so they navigate");
ok(menu[0].value.indexOf("Ch 1") === 0, "index shows the channel");
ok(hierarchy.levels.ccmap_fx1.menu[0].action.indexOf("cc_edit:") === 0,
   "a parameter row opens the card, and carries which row it is");

process.exit(fail ? 1 : 0);
'
