#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# `paginate: false` — a level is ONE page, however long.
#
# Eight is the number of physical KNOBS: a grid page has eight cells and nowhere
# to put a ninth. A LIST has no such limit — it draws five rows of a page and
# scrolls the rest, and knobRows() reads a page's keys with no cap. Global
# Settings is pinned to the list and was still being planned as a grid, so a
# ninth param in a section silently became a second page named "<Section> - 2"
# holding one row.
#
# Two things must hold, and they pull in opposite directions:
#
#   - with the flag, an over-length level stays one page
#   - WITHOUT it, chunking is exactly as it was — every module in the fleet
#     depends on that, and a default that quietly changed would rearrange 95
#     modules' pages with no error anywhere
#
# So the default is asserted as hard as the new behaviour is.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const { planPages, KNOBS_PER_PAGE, PAGE_KNOBS } =
    await import(R + "/src/shared/param_pages/page_plan.mjs");

/* A level of `n` params, declared the way a real contract declares one. */
function contractOf(n, level = "audio") {
    const keys = [];
    for (let i = 0; i < n; i++) keys.push("p" + i);
    return {
        hierarchy: { levels: { [level]: { name: "Audio", params: keys.map((k) => ({ key: k })) } } },
        chainParams: keys.map((k) => ({ key: k, name: k, type: "bool" })),
    };
}
const pagesOf = (n, opts) => {
    const c = contractOf(n);
    return planPages({ hierarchy: c.hierarchy, chainParams: c.chainParams, ...opts })
        .pages.filter((p) => p.kind === PAGE_KNOBS && p.level === "audio");
};

/* ---- 1. the DEFAULT still chunks at the grid page ---------------------- */

/* Asserted first and across a range, because this is the behaviour 95 modules
 * already have. A regression here is silent: pages simply regroup. */
for (const [n, want] of [[1, 1], [8, 1], [9, 2], [16, 2], [17, 3]]) {
    const got = pagesOf(n).length;
    if (got !== want) {
        fail("default: a level of " + n + " params planned " + got + " pages, expected " +
             want + " — the grid holds " + KNOBS_PER_PAGE + " and every module depends on it");
    }
}
{
    /* And no page may exceed the eight cells a grid actually has. */
    for (const p of pagesOf(17)) {
        const keys = (p.keys || []).filter(Boolean);
        if (keys.length > KNOBS_PER_PAGE) {
            fail("default: planned a grid page of " + keys.length + " keys — there are only " +
                 KNOBS_PER_PAGE + " knobs");
        }
    }
    /* Explicit true is the same as absent. */
    if (pagesOf(9, { paginate: true }).length !== 2) {
        fail("paginate: true is not the same as omitting it");
    }
}

/* ---- 2. paginate:false makes a level ONE page, however long ------------ */

for (const n of [1, 8, 9, 17, 40]) {
    const pages = pagesOf(n, { paginate: false });
    if (pages.length !== 1) {
        fail("paginate:false: a level of " + n + " params planned " + pages.length +
             " pages — a section is one scrolling list");
    }
    const keys = pages.length ? (pages[0].keys || []).filter(Boolean) : [];
    if (keys.length !== n) {
        fail("paginate:false: a level of " + n + " params kept " + keys.length +
             " keys on its page — the rest were dropped, not paginated");
    }
}

/* No page is named "<Section> - 2": the split is what put a jog step in the
 * middle of a list, and the NAME is how it showed up on the device. */
{
    const c = contractOf(17);
    const all = planPages({ hierarchy: c.hierarchy, chainParams: c.chainParams, paginate: false }).pages;
    const split = all.filter((p) => / - \d+$/.test(String(p.name)));
    if (split.length) {
        fail("paginate:false still produced numbered pages: " + split.map((p) => p.name).join(", "));
    }
}

/* ---- 3. ORDER is preserved -------------------------------------------- */

/* Joining pages back together is not the same as never splitting them if the
 * keys come back in a different order — the rows are a declared sequence. */
{
    const pages = pagesOf(20, { paginate: false });
    const keys = pages.length ? (pages[0].keys || []).filter(Boolean) : [];
    const want = [];
    for (let i = 0; i < 20; i++) want.push("p" + i);
    if (keys.join(",") !== want.join(",")) {
        fail("paginate:false reordered the level: got " + keys.join(",") );
    }
}

/* ---- 4. the controller carries it, and carries it to the REPLANS ------- */

/* The two replan paths (mode change, visible_if change) call planPages again
 * from controller state. They already had to remember `mode` and `visible`;
 * this is the third thing, and forgetting it would re-split the page the first
 * time a condition changed — long after entry, which is the hardest kind of
 * regression to attribute. Asserted against the source, since driving a live
 * controller needs a device. */
{
    const FS = await import("node:fs");
    const src = FS.readFileSync(R + "/src/shared/param_pages/page_controller.mjs", "utf8");

    const calls = [...src.matchAll(/planPages\(\{[\s\S]*?\}\)/g)].map((m) => m[0]);
    if (calls.length < 3) {
        fail("expected at least 3 planPages call sites in page_controller.mjs, found " + calls.length);
    }
    calls.forEach((c, i) => {
        if (!/paginate\s*:/.test(c)) {
            fail("planPages call site " + (i + 1) + " in page_controller.mjs does not pass paginate — " +
                 "that plan will re-chunk a list contract:\n" + c.slice(0, 200));
        }
    });

    if (!/function load\(\{[^}]*paginate/.test(src)) {
        fail("controller load() does not accept paginate");
    }
    if (!/s\.paginate\s*=/.test(src)) {
        fail("controller load() does not record paginate on the state, so the replans cannot see it");
    }
}

/* ---- 5. the hand-off from the screen ---------------------------------- */

{
    const FS = await import("node:fs");
    const pp = FS.readFileSync(R + "/src/shadow/shadow_ui_param_pages.mjs", "utf8");
    const ui = FS.readFileSync(R + "/src/shadow/shadow_ui.js", "utf8");

    if (!/export function paramPagesPaginate\(/.test(pp)) {
        fail("paramPagesPaginate is gone from shadow_ui_param_pages.mjs");
    }
    if (!/paginate:\s*paramPagesPaginate\(\)/.test(pp)) {
        fail("controller.load is not given paramPagesPaginate() — the chrome flag reaches nothing");
    }
    /* It must read the CHROME, not the layout: paramPagesLayout() returns
     * LAYOUT_LIST with the screen reader on or Param View set to List, and
     * inferring pagination from that would regroup every module. */
    const body = pp.match(/export function paramPagesPaginate\(\)\s*\{([\s\S]*?)\n\}/);
    if (!body) {
        fail("could not read the body of paramPagesPaginate");
    } else {
        if (!/currentChrome/.test(body[1])) {
            fail("paramPagesPaginate does not read the chrome");
        }
        if (/paramPagesLayout|LAYOUT_LIST|tts_get_enabled|param_view_get_mode/.test(body[1])) {
            fail("paramPagesPaginate is derived from the LAYOUT or a user preference — " +
                 "it must be a property of the contract, or every module regroups when " +
                 "the screen reader is switched on");
        }
    }

    /* And Global Settings is the contract that sets it. */
    const entry = ui.match(/function enterGlobalSettingsGrid\([\s\S]*?\n\}/);
    if (!entry) {
        fail("enterGlobalSettingsGrid is gone from shadow_ui.js");
    } else if (!/paginate:\s*false/.test(entry[0])) {
        fail("enterGlobalSettingsGrid does not pass paginate: false — a ninth param in a " +
             "section silently becomes a second page named \"<Section> - 2\"");
    }
}

if (failures) process.exit(1);
console.log("PASS: page plan pagination — the default still chunks at " + KNOBS_PER_PAGE +
    " (1/8/9/16/17 params -> 1/1/2/2/3 pages, none over the cell count), paginate:false " +
    "keeps a level of any length on ONE page in declared order with no numbered names, " +
    "all three controller call sites and both replans carry it, and it is read from the " +
    "CHROME rather than derived from the layout");
'
