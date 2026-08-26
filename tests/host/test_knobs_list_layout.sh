#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# PAGE_KNOBS AS A LIST — the layout, and the proof it is only a layout.
#
# §4 of docs/superpowers/specs/2026-08-23-one-list-engine-design.md. The knob
# grid is the one page kind with no list rendering, which is the only reason
# choosing `param_view = List` forks users into a second engine instead of a
# second arrangement of the same one. LAYOUT_LIST closes that.
#
# The governing constraint is "one thing changed in one place", so the central
# assertion here is SURFACE AGREEMENT WITH NO EXCEPTIONS: for every PAGE_KNOBS
# param in the fleet fixture, the value string the list shows is the value
# string the grid shows.
#
# THERE IS DELIBERATELY NOWHERE TO PUT AN ALLOW-LIST. An exceptions table is
# how §2's single-source table grows a second column: each entry is individually
# reasonable and the sum of them is two engines again. Where a value legitimately
# has a short form and a long one, that is `short_options` — one declaration,
# two renderings — and this file asserts the mechanism rather than exempting the
# key that uses it.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

# ---------------------------------------------------------------------------
# NO SECOND DEFINITION (design test 7.4), asserted against src/ rather than
# against a hand-kept list, so a new copy is caught on arrival.
# ---------------------------------------------------------------------------
static_failures=0

# THE value string has one definition. It lives with the renderer because the
# grid's label band and header strip are its other two consumers; the list
# reaches it by import.
n=$(command grep -rn "^export function displayValue\b\|^function displayValue\b" src | wc -l | tr -d ' ')
if [ "$n" != "1" ]; then
    echo "FAIL: displayValue has ${n} definitions in src/, expected exactly 1"
    command grep -rn "function displayValue" src || true
    static_failures=$((static_failures + 1))
fi

# The controller must not reach past it to the raw numeric formatter. That is
# how the "--" for an unread value and the tail of a filepath would come back
# as "0.00" on one surface and a path on the other -- the exact defect the
# displayValue wrapper exists to prevent, one import lower down.
if command grep -q "formatParamValue" src/shared/param_pages/page_controller.mjs; then
    echo "FAIL: page_controller.mjs imports formatParamValue -- a second value formatter"
    static_failures=$((static_failures + 1))
fi

# `short_options` is for the three-character enum square and nothing else. If it
# is ever consulted outside the widget that draws that square, the long form
# stops being the thing every roomy surface shows.
# (Property access only -- the prose above the code is allowed to name it.)
n=$( { command grep -rn "\.short_options\|short_options\[" src/shared/param_pages/page_controller.mjs || true; } | wc -l | tr -d ' ')
if [ "$n" != "0" ]; then
    echo "FAIL: page_controller.mjs consults short_options (${n} sites); it belongs to the enum square alone"
    static_failures=$((static_failures + 1))
fi

# ROWS ARE DRAWN BY THE ONE LIST. drawMenuList is called from exactly one place
# in this file -- drawPageChromeList -- so the knobs layout cannot have grown a
# row loop of its own.
n=$(command grep -c "drawMenuList(" src/shared/param_pages/page_controller.mjs | tr -d ' ')
if [ "$n" != "1" ]; then
    echo "FAIL: page_controller.mjs calls drawMenuList ${n} times, expected exactly 1 (inside drawPageChromeList)"
    static_failures=$((static_failures + 1))
fi

# NO NEW LIST GEOMETRY. The rect, the stride and the frame are already defined;
# the list layout uses them. Derived by listing what the file declares rather
# than by grepping for a name someone would have had to think of.
# One `const` line can declare several names, so every NAME = on such a line
# counts -- MENU_FRAME_X/Y/W share one.
declared=$(command grep "^const [A-Z][A-Z0-9_]* = " src/shared/param_pages/page_controller.mjs \
           | command grep -o "[A-Z][A-Z0-9_]* = " | sed 's/ = //' | sort -u | tr '\n' ' ')
# EMPTY_SLOTS is not geometry: it is the frozen "nothing is held on this page"
# array the slide hands to a page that is not the current one. Listed rather
# than exempted by pattern, so the pin keeps its whole job.
expected="EMPTY_SLOTS MENU_BRACKET_LEN MENU_FRAME_BOTTOM_INSET MENU_FRAME_W MENU_FRAME_X MENU_FRAME_Y MENU_LIST_INDICATOR_X TRIGGER_BURST_KEEP_MS TRIGGER_BURST_MAX "
if [ "$declared" != "$expected" ]; then
    echo "FAIL: page_controller.mjs's module constants changed."
    echo "  declared: ${declared}"
    echo "  expected: ${expected}"
    echo "  A new geometry constant here is a second definition of a rect that"
    echo "  already exists (MENU_LIST_X/Y/W, imported from render_page_movy)."
    static_failures=$((static_failures + 1))
fi

if [ "$static_failures" != "0" ]; then exit 1; fi

node --input-type=module -e '
const R = process.cwd();
const fs = await import("node:fs");

const { createController, LAYOUT_LIST, LAYOUT_MOVY } =
    await import(R + "/src/shared/param_pages/page_controller.mjs");
const { displayValue } = await import(R + "/src/shared/param_pages/render_page_movy.mjs");
const { planPages, PAGE_KNOBS } = await import(R + "/src/shared/param_pages/page_plan.mjs");
const { buildMetaIndex, KIND_ENUM, KIND_OPAQUE, isTurnable } =
    await import(R + "/src/shared/param_pages/param_meta.mjs");
const { createFakeDevice } = await import(R + "/tools/param-pages/fake_device.mjs");
const { FIXTURE, fakeValue } = await import(R + "/tools/param-pages/cases.mjs");
const { createFramebuffer, drawContext } = await import(R + "/tools/param-pages/harness.mjs");

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const eq = (a, b, m) => { if (a !== b) fail(`${m}: got ${JSON.stringify(a)}, want ${JSON.stringify(b)}`); };

const fleet = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));

/* ---- 1. LAYOUT_LIST exists and is its own layout ---------------------- */
if (typeof LAYOUT_LIST !== "string" || !LAYOUT_LIST) fail("LAYOUT_LIST is not exported as a string");
if (LAYOUT_LIST === LAYOUT_MOVY) fail("LAYOUT_LIST must not be the same value as LAYOUT_MOVY");

/* A controller pointed at one fixture module, its values already read in. */
function boot(mod, { layout = LAYOUT_LIST, initial = {}, pageIndex = null } = {}) {
    const dev = createFakeDevice({ id: mod.id, prefix: "synth", initial });
    const ctrl = createController({
        getParam: dev.getParam, setParam: dev.setParam,
        announce: (t) => dev.announce(t), now: dev.now,
    });
    ctrl.load({ prefix: "synth" });
    ctrl.setLayout(layout);
    if (pageIndex !== null) ctrl.goToPage(pageIndex, { remember: false });
    /* One read per tick is the whole point of the staggered cursor. */
    const n = ((ctrl.page && ctrl.page.keys) || []).length + 3;
    for (let i = 0; i < n; i++) ctrl.tick();
    return { dev, ctrl };
}

/* =======================================================================
 * 2. SURFACE AGREEMENT, EVERY PARAM, NO EXCEPTIONS.
 *
 * The grids value string is displayValue(raw, meta) -- what the cells label
 * band prints under a held knob, and what the held-knob header strip prints.
 * Compared against the UNFITTED form deliberately: the cell then squeezes it
 * into 30px and the list into ~90, and a comparison of the two FITTED strings
 * would be measuring the two rectangles rather than the two readings.
 * ======================================================================= */
let checked = 0, modulesSeen = 0;
for (const mod of fleet.modules) {
    const { pages } = planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const metaIndex = buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const knobPages = pages.map((p, i) => ({ p, i })).filter(({ p }) => p.kind === PAGE_KNOBS);
    if (!knobPages.length) continue;
    modulesSeen++;

    for (const { p, i } of knobPages) {
        /*
         * DISCRIMINATING values, not the fake device s default of min for
         * everything. A filepath seeded "0" formats identically however it is
         * formatted, so a mutation that skipped displayValue s tail-of-the-path
         * rule passed the whole sweep -- the fixture was agreeing about a value
         * that could not disagree. fakeValue gives an opaque param a real path
         * and everything else a mid-range reading with its declared unit.
         */
        const initial = {};
        for (const k of (p.keys || [])) {
            if (k) initial[k] = fakeValue(k, metaIndex.getOrGuess(k));
        }
        const { ctrl } = boot(mod, { pageIndex: i, initial });
        /* A page the planner produced but the controller re-planned away
         * (visible_if) is not this tests business; only compare what is on
         * screen right now. */
        const live = ctrl.page;
        if (!live || live.kind !== PAGE_KNOBS) continue;
        const rows = ctrl.knobRows();
        const entries = ctrl.knobListEntries();
        if (rows.length !== entries.length) {
            fail(`${mod.id} page ${i}: ${rows.length} rows but ${entries.length} entries`);
            continue;
        }
        for (let r = 0; r < rows.length; r++) {
            const key = rows[r].key;
            const meta = ctrl.metaIndex.getOrGuess(key);
            const raw = ctrl.state.values[key] === undefined ? null : ctrl.state.values[key];
            const grid = displayValue(raw, meta);
            if (entries[r].value !== grid) {
                fail(`${mod.id} page ${i} "${key}": list shows ${JSON.stringify(entries[r].value)}, `
                     + `grid shows ${JSON.stringify(grid)}`);
            }
            /* The row must also NAME the param. The five-character cell
             * mnemonic is a property of a 32px cell, never of a row. */
            eq(entries[r].name, meta.label || meta.key, `${mod.id} page ${i} "${key}" row label`);
            checked++;
        }
    }
}
if (checked < 500) fail(`only ${checked} params compared across ${modulesSeen} modules -- the sweep is not running`);

/* =======================================================================
 * 3. short_options SHORTENS THE SQUARE, NEVER THE LIST.
 *
 * §5.5: "any value too long for a cell is a short_options entry, never a second
 * code path". The list is a surface with room, so it gets the LONG option --
 * asserted explicitly here rather than by exempting the keys that declare one,
 * because an exemption would make the divergence legal instead of pinning it.
 * ======================================================================= */
{
    /* A synthetic contract, so this holds whatever the fleet happens to
     * declare this week -- a fixture-derived case would silently stop testing
     * anything the day the last short_options in the fleet was removed. */
    const meta = {
        key: "usbc_out_persist", type: "enum",
        options: ["Off", "On (Main Out)"],
        short_options: ["OFF", "ON"],
    };
    /* displayValue never looks at short_options; that is the assertion. */
    eq(displayValue("1", meta), "On (Main Out)", "long option on a roomy surface");
    eq(displayValue("0", meta), "Off", "long option, index 0");
    if (displayValue("1", meta) === meta.short_options[1]) {
        fail("displayValue returned the SHORT option -- short_options has leaked past the enum square");
    }
    const src = fs.readFileSync(R + "/src/shared/param_pages/render_page_movy.mjs", "utf8");
    const body = src.slice(src.indexOf("export function displayValue"),
                           src.indexOf("export function displayValue") + 400);
    if (body.includes("short_options")) fail("displayValue consults short_options");
}

/* =======================================================================
 * 4. THE JOG EDITS THROUGH THE GRID S OWN TURN.
 *
 * Two controllers on the same module: one on the grid turning knob N, one on
 * the list jogging in edit mode on the row that came from knob N. Identical
 * write sequences is the only assertion that cannot be satisfied by a
 * carefully-matching parallel implementation.
 * ======================================================================= */
{
    const mod = fleet.modules.find((m) => m.id === "obxd");
    const grid = boot(mod, { layout: LAYOUT_MOVY, pageIndex: null });
    const list = boot(mod, { layout: LAYOUT_LIST, pageIndex: null });
    /* Same page in both. */
    const target = grid.ctrl.pages.findIndex((p) => p.kind === PAGE_KNOBS);
    grid.ctrl.goToPage(target, { remember: false });
    list.ctrl.goToPage(target, { remember: false });
    for (let i = 0; i < 12; i++) { grid.ctrl.tick(); list.ctrl.tick(); }

    const rows = list.ctrl.knobRows();
    const SLOT = rows[2].slot;      /* third row, whatever knob it came from */
    list.ctrl.enterMenu();
    list.ctrl.onJog(1); list.ctrl.onJog(1);          /* row 0 -> row 2 */
    eq(list.ctrl.knobRowIndex(), 2, "jog moved the row cursor");
    eq(list.ctrl.state.knobEditing, false, "moving the cursor is not editing");
    eq(list.dev.writes.length, 0, "moving the cursor wrote to the device");

    list.ctrl.onClick(0);
    eq(list.ctrl.state.knobEditing, true, "click on a turnable row opens it for editing");

    grid.dev.resetCounters(); list.dev.resetCounters();
    for (let i = 0; i < 6; i++) {
        grid.dev.advance(50); list.dev.advance(50);
        grid.ctrl.onKnobTurn(SLOT, 1);
        list.ctrl.onJog(1);
    }
    const gw = JSON.stringify(grid.dev.writes);
    const lw = JSON.stringify(list.dev.writes);
    if (gw !== lw) fail(`jog-edit writes differ from knob-turn writes:\n  knob ${gw}\n  jog  ${lw}`);
    if (grid.dev.writes.length === 0) fail("the control case wrote nothing -- the comparison is vacuous");

    /* Back steps out one level at a time. */
    list.ctrl.exitMenu();
    eq(list.ctrl.state.knobEditing, false, "Back leaves edit mode first");
    eq(list.ctrl.menuEntered(), true, "...and stays in the list");
    list.ctrl.exitMenu();
    eq(list.ctrl.menuEntered(), false, "a second Back leaves the list");
}

/* =======================================================================
 * 5. A KNOB PAGE IS A DOOR ONLY WHEN IT IS A LIST.
 *
 * The jog means one thing everywhere -- it pages -- until a click says
 * otherwise. On the grid there is nothing to enter and this must not change.
 * ======================================================================= */
{
    const mod = fleet.modules.find((m) => m.id === "obxd");
    const { ctrl } = boot(mod, { layout: LAYOUT_LIST });
    const first = ctrl.pageIndex;
    eq(ctrl.menuEntered(), false, "a knob list is inert on arrival");
    ctrl.onJog(1);
    if (ctrl.pageIndex === first) fail("un-entered, the jog must still PAGE");

    const g = boot(mod, { layout: LAYOUT_MOVY });
    const gfirst = g.ctrl.pageIndex;
    eq(g.ctrl.enterMenu(), false, "a knob GRID has nothing to enter");
    g.ctrl.onJog(1);
    if (g.ctrl.pageIndex === gfirst) fail("the grid jog stopped paging");
}

/* =======================================================================
 * 6. WHAT A ROW DOES FOR AN OPAQUE PARAM.
 *
 * The same thing the cell does: it is not turnable, so the jog never edits it,
 * and the click hands the host the same "open" intent the grid hands it. No new
 * interaction was invented for this layout.
 * ======================================================================= */
{
    const mod = fleet.modules.find((m) => m.id === "breakbeat");
    const { pages } = planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const at = pages.findIndex((p) => p.kind === PAGE_KNOBS &&
        (p.keys || []).some((k) => k === "A_sample_path"));
    const { dev, ctrl } = boot(mod, {
        pageIndex: at, initial: { A_sample_path: "/data/UserData/Samples/kick_01.wav" } });
    const rows = ctrl.knobRows();
    const opaqueRow = rows.findIndex((r) => r.key === "A_sample_path");
    if (opaqueRow < 0) fail("breakbeat page has no A_sample_path row");

    const meta = ctrl.metaIndex.getOrGuess("A_sample_path");
    eq(meta.kind, KIND_OPAQUE, "A_sample_path is opaque");
    eq(isTurnable(meta), false, "an opaque param is not turnable");
    eq(ctrl.knobListEntries()[opaqueRow].value, "kick_01.wav",
       "the row shows the tail, exactly as the cell does");

    ctrl.enterMenu();
    for (let i = 0; i < opaqueRow; i++) ctrl.onJog(1);
    eq(ctrl.knobRowIndex(), opaqueRow, "cursor on the opaque row");
    dev.resetCounters();
    ctrl.onClick(0);
    eq(ctrl.state.knobEditing, false, "an opaque row is never opened for jog editing");
    const pend = ctrl.takePending();
    if (!pend || pend.action !== "open") fail("clicking an opaque row must hand the host an open intent");
    else eq(pend.key, "A_sample_path", "the open intent names the row param");
    eq(dev.writes.length, 0, "clicking an opaque row wrote to the device");

    /* And the jog, with that row focused and nothing entered for editing,
     * still only moves the cursor. */
    const before = ctrl.knobRowIndex();
    ctrl.onJog(before > 0 ? -1 : 1);   /* whichever direction has room */
    if (ctrl.knobRowIndex() === before) fail("the jog stopped moving the cursor");
    eq(dev.writes.length, 0, "the jog wrote to a param it cannot turn");
}

/* =======================================================================
 * 7. AN ENUM ROW OPENS THE SAME PICKER THE CELL OPENS.
 * ======================================================================= */
{
    const mod = fleet.modules.find((m) => m.id === "arp");
    const { ctrl } = boot(mod);
    const rows = ctrl.knobRows();
    const at = rows.findIndex((r) => {
        const m = ctrl.metaIndex.getOrGuess(r.key);
        return m.kind === KIND_ENUM && Array.isArray(m.options) && m.options.length > 1;
    });
    if (at < 0) fail("arp has no listable enum on its first page");
    else {
        ctrl.enterMenu();
        for (let i = 0; i < at; i++) ctrl.onJog(1);
        ctrl.onClick(0);
        eq(ctrl.state.knobEditing, false, "an enum row opens its picker, not edit mode");
        const pend = ctrl.takePending();
        if (!pend || pend.action !== "open") fail("clicking an enum row must open the picker");
        else if (!Array.isArray(pend.options)) fail("the enum intent carries no options");
    }
}

/* =======================================================================
 * 8. A SPARSE PAGE. Rows carry the SLOT they came from, so an edit still
 * reaches the right knob when the page has holes.
 * ======================================================================= */
{
    const mod = fleet.modules.find((m) => m.id === "branchage");
    if (mod) {
        const { pages } = planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
        const at = pages.findIndex((p) => p.kind === PAGE_KNOBS &&
            (p.keys || []).length > (p.keys || []).filter(Boolean).length);
        if (at >= 0) {
            const { ctrl } = boot(mod, { pageIndex: at });
            const rows = ctrl.knobRows();
            const keys = ctrl.page.keys;
            eq(rows.length, keys.filter(Boolean).length, "a sparse page lists only its real params");
            for (const r of rows) eq(keys[r.slot], r.key, `row for slot ${r.slot} names its own key`);
        }
    }
}

/* =======================================================================
 * 9. NO DOUBLE-ANNOUNCING.
 *
 * The controller announces every move itself, with position and value; letting
 * drawMenuList announce as well says everything twice to a screen-reader user.
 * A draw must therefore be silent.
 * ======================================================================= */
{
    const mod = fleet.modules.find((m) => m.id === "obxd");
    const dev = createFakeDevice({ id: "obxd", prefix: "synth" });
    const spoken = [];
    const ctrl = createController({
        getParam: dev.getParam, setParam: dev.setParam,
        announce: (t) => spoken.push(t), now: dev.now,
    });
    ctrl.load({ prefix: "synth" });
    ctrl.setLayout(LAYOUT_LIST);
    for (let i = 0; i < 12; i++) ctrl.tick();
    ctrl.enterMenu();

    const fb = createFramebuffer();
    const ctx = drawContext(fb);
    spoken.length = 0;
    for (let i = 0; i < 5; i++) ctrl.render(ctx, { title: "T1 > OB-XD", footer: [["JOG", "PG"]] });
    if (spoken.length !== 0) fail(`rendering announced ${spoken.length} times: ${JSON.stringify(spoken)}`);

    /* Moving the cursor announces exactly ONCE, and says the name, the value
     * and the position -- announceTouch plus a position, i.e. what touching
     * that knob on the grid says. */
    spoken.length = 0;
    ctrl.onJog(1);
    eq(spoken.length, 1, "one announcement per cursor move");
    if (spoken.length === 1 && !/\b2 of \d+$/.test(spoken[0]))
        fail(`the row announcement carries no position: ${JSON.stringify(spoken[0])}`);

    /* And nothing is drawn outside the display. */
    if (fb.clipped() > 0) fail(`the list layout drew ${fb.clipped()} px outside the display`);
}

/* =======================================================================
 * 10. NO DEVICE READS ON THE DRAW PATH.
 *
 * A param read is ~2.8ms against a 1.68ms whole-page render, so a value read
 * while drawing costs more than redrawing the screen. The grid draws from the
 * staggered cursor s cache; so must the list.
 * ======================================================================= */
{
    const mod = fleet.modules.find((m) => m.id === "obxd");
    const { dev, ctrl } = boot(mod);
    ctrl.enterMenu();
    const fb = createFramebuffer();
    const ctx = drawContext(fb);
    dev.resetCounters();
    for (let i = 0; i < 10; i++) ctrl.render(ctx, { title: "T1", footer: [["JOG", "PG"]] });
    if (dev.reads.length !== 0)
        fail(`10 renders issued ${dev.reads.length} device reads: ${JSON.stringify(dev.reads.slice(0, 6))}`);
}

/* =======================================================================
 * A PLAIN CLICK MUST ENTER THE LIST, NOT OPEN THE SECTION PICKER.
 *
 * page_input.mjs kept its OWN copy of "which pages are doors", spelled as a
 * literal list of kinds ("menu" || "preset" || "items"). When PAGE_KNOBS
 * became a door in the list layout, only the copy inside the controller learned it --
 * so a plain click fell past that branch to the no-knob-held one and opened
 * the SECTION PICKER. The list could not be entered at all, and nothing
 * failed: the page was simply inert. Reported from the device.
 *
 * Driven through the REAL input path (decodeInput -> applyInput), because that
 * is where the second definition lived. Calling controller.onClick directly
 * passes with the bug still present, which is exactly why it survived.
 * ======================================================================= */
{
    const PI = await import(R + "/src/shared/param_pages/page_input.mjs");
    const mod = fleet.modules.find((m) => (m.chain_params || []).length >= 2)
             || fleet.modules[0];
    const { ctrl } = boot(mod, { layout: LAYOUT_LIST });

    const p0 = ctrl.page;
    if (!p0 || p0.kind !== "knobs") fail("expected a PAGE_KNOBS page to click on");
    if (!ctrl.isDoor || !ctrl.isDoor())
        fail("a knobs page in LAYOUT_LIST must report as a door");
    if (ctrl.menuEntered()) fail("the page must start un-entered");

    /* CC 3 = jog click; 127 = press. */
    const intent = PI.decodeInput(new Uint8Array([0xb0, 3, 127]), {});
    if (!intent || intent.type !== "click") fail("CC3/127 did not decode as a click");
    PI.applyInput(ctrl, intent, { nowMs: 0 });

    if (ctrl.pickerOpen)
        fail("a plain click opened the SECTION PICKER -- page_input is not asking "
           + "the controller which pages are doors");
    if (!ctrl.menuEntered())
        fail("a plain click did not enter the knobs list");
}

if (failures) { console.error(`\n${failures} failure(s)`); process.exit(1); }
console.log(`PASS: knobs-as-list layout (${checked} params compared across ${modulesSeen} modules), and a plain click enters it`);
'
