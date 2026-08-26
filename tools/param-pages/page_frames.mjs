/*
 * page_frames.mjs — the frames the page-slide refactor must not have changed.
 *
 * WHY THIS IS A SEPARATE FILE RATHER THAN LIVING IN THE TEST.
 *
 * "Byte-identical to before the refactor" cannot be asserted by comparing the
 * new drawPage against the new render(), because render() CALLS drawPage --
 * both sides move together and the assertion is a tautology with respect to
 * the thing it names. That is not hypothetical: with the comparison written
 * that way, flipping drawPage`s `chrome` default from true to false stripped
 * the bank bar and the footer from every page of the real UI and all 35
 * assertions still passed.
 *
 * So the baseline has to come from code that predates the refactor. This
 * module is the scenario driver, kept out of the test so it can be run
 * UNCHANGED inside a worktree checked out at the parent commit -- the same
 * driver, two controllers, one pair of frame sets. It touches only the public
 * controller surface (createController / setLayout / load / tick / goToPage /
 * render), which is what lets it run against both.
 *
 * Regenerate the baseline (from the PARENT of the refactor commit, never from
 * the current tree -- regenerating from the current tree destroys the evidence
 * and turns the test back into a tautology):
 *
 *   git worktree add /tmp/pgbase <parent-sha> --detach
 *   cp tools/param-pages/page_frames.mjs /tmp/pgbase/tools/param-pages/
 *   (cd /tmp/pgbase && node tools/param-pages/page_frames.mjs) \
 *       > tests/fixtures/page-render-baseline.txt
 *   git worktree remove /tmp/pgbase --force
 *
 * A legitimate change to what a page LOOKS like will fail the comparison, the
 * same bargain tests/fixtures/movy-geom-baseline.txt already takes. Update it
 * deliberately, and only ever from a commit that does not contain the change
 * being tested.
 */

import { createController, LAYOUT_MOVY, LAYOUT_LIST }
    from "../../src/shared/param_pages/page_controller.mjs";
import { createFramebuffer, drawContext } from "./harness.mjs";
import { createHash } from "node:crypto";
import { pathToFileURL } from "node:url";

/*
 * A HASH per scenario, not the frame.
 *
 * A 128x64 frame is 8192 bytes and base64 makes it ~11KB; 42 of them is a
 * 450KB fixture of mostly-zeros, which is a lot of repo for a yes/no question.
 * The gate here IS yes/no -- did any pixel move -- and the scenario NAME is
 * what tells you where to look when the answer is no. Re-run this file against
 * both trees to see the actual pixels.
 */
const digest = (fb) => createHash("sha256").update(Buffer.from(fb.pixels)).digest("hex");

/*
 * One fixture covering all four page kinds. A knob-only fixture would leave
 * four of the five draw paths unmeasured -- page_controller.mjs draws the bank
 * bar from one call site per kind, plus the knobs-as-list fork.
 */
const KEYS = [];
for (let i = 0; i < 24; i++) KEYS.push("p" + i);

/*
 * ADSR + an enum + a filter pair, so resolveViz actually RESOLVES something.
 *
 * 24 plain floats produce no groups at all, which left the `viz:` plumbing
 * threaded through drawPage in none of the recorded frames -- the fixture would
 * have been green with that option dropped entirely. attack/decay/sustain/
 * release are adjacent so the envelope detector takes them as one run, and
 * cutoff/resonance give the filter detector its corroborated pair.
 */
const VIZ_KEYS = ["attack", "decay", "sustain", "release",
                  "cutoff", "resonance", "lfo_shape", "sync"];

export const CHAIN_PARAMS = [
    ...KEYS.map((k, i) => ({
        key: k, name: "P" + i, type: "float", min: 0, max: 1, step: 0.01,
    })),
    ...["attack", "decay", "sustain", "release", "cutoff", "resonance"].map((k) => ({
        key: k, name: k[0].toUpperCase() + k.slice(1),
        type: "float", min: 0, max: 1, step: 0.01,
    })),
    { key: "lfo_shape", name: "Shape", type: "enum",
      options: ["Sine", "Tri", "Saw", "Square"] },
    { key: "sync", name: "Sync", type: "enum", options: ["Off", "On"] },
    /* The child level`s parameters. Metadata stays keyed by the BARE name --
     * only the wire key is rewritten by childResolve -- so they are declared
     * unprefixed here, which is what the real contracts do. */
    ...["tune", "level"].map((k) => ({
        key: k, name: k[0].toUpperCase() + k.slice(1),
        type: "float", min: 0, max: 1, step: 0.01,
    })),
];

export const HIER = {
    modes: null,
    levels: {
        root: {
            label: "T",
            knobs: KEYS,
            params: [
                ...KEYS.map((k) => ({ key: k })),
                { level: "shape", label: "Shape" },
                { level: "parts", label: "Parts" },
            ],
            /* A MENU page. */
            menu: [{ level: "stuff", label: "Stuff" }],
            /* A PRESET page. */
            list_param: "preset",
            count_param: "preset_count",
            name_param: "preset_name",
        },
        /* An ITEMS page. */
        stuff: { label: "Stuff", items_param: "thing_list", select_param: "thing_index" },
        /* A knob page whose graphics are REAL: an envelope, a filter pair and
         * two enum squares. */
        shape: {
            label: "Shape",
            knobs: VIZ_KEYS,
            params: VIZ_KEYS.map((k) => ({ key: k })),
        },
        /*
         * A CHILD LEVEL. pageLabel(mp) is the one signature this refactor
         * changed -- it is what forced the test_child_levels.sh re-pin -- and
         * its non-trivial branch (childLevel, s.pages.filter, childIndexFor)
         * is only reachable from a level like this one. Without it the fixture
         * exercised pageLabel`s `return pg.name` line and nothing else.
         */
        parts: {
            label: "Parts",
            child_count: 4,
            child_prefix: "part",
            child_label: "Part",
            knobs: ["tune", "level"],
            params: [{ key: "tune" }, { key: "level" }],
        },
    },
};

export const FOOTER = [["CLK", "OPEN"]];
export const TITLE = "T";

/*
 * A fixed clock. The renderer reads `now` off its options for the widget
 * animations, so a wall clock would make two runs of the same scenario differ
 * for reasons that have nothing to do with the refactor.
 */
export function makeStore() {
    const store = {};
    for (const k of KEYS) store[k] = "0.5";
    for (const k of VIZ_KEYS) store[k] = "0.5";
    store.lfo_shape = "1";
    store.sync = "1";
    /* Both the bare and the child-resolved forms: childResolve rewrites the
     * WIRE key, so a child page reads `part1_tune` while its metadata is still
     * keyed `tune`. */
    for (const k of ["tune", "level"]) {
        store[k] = "0.5";
        /* resolveChildKey`s index is 0-based (`<prefix><index>_<key>`), so the
         * four instances are part0_ .. part3_ and NOT part1_ .. part4_. */
        for (let i = 0; i < 4; i++) store[`part${i}_${k}`] = String(0.2 + 0.15 * i);
    }
    store.preset = "0";
    store.preset_count = "12";
    store.preset_name = "Fat Bass";
    store.thing_list = JSON.stringify(["Alpha", "Beta", "Gamma"]);
    store.thing_index = "1";
    return store;
}

/*
 * `extraIo` lets a caller drive an explicit slide duration. The BASELINE
 * driver must never pass one: it has to run unchanged against the
 * pre-refactor tree, which knows nothing about slides, and the frames it
 * records are settled ones either way.
 */
export function makeController(clockRef, store, extraIo) {
    return createController({
        ...(extraIo || {}),
        getParam: (k) => {
            const b = String(k).replace(/^[^:]+:/, "");
            if (b === "ui_hierarchy") return JSON.stringify(HIER);
            if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
            return b in store ? store[b] : "";
        },
        setParam: () => {},
        announce: () => {},
        now: () => clockRef.t,
    });
}

/**
 * Render every page of the fixture, in both layouts, with the DEFAULT render
 * options -- which is the whole real-UI path. Returns [{ name, frame }].
 *
 * The default options matter: a caller of render() has no way to ask for
 * chrome, so if the bank bar or the footer stopped being drawn here, they
 * stopped being drawn on the device.
 */

/*
 * LET THE SLIDE FINISH INSIDE THE TICKS WE ALREADY TAKE.
 *
 * A page change starts a slide, so a frame captured too early is a MID-SLIDE
 * composite and every baseline hash moves at once -- which reads as a
 * rendering regression and is a still-moving page.
 *
 * The obvious fix, ticking until it settles, is WRONG here: every tick is also
 * one parameter read, so extra ticks populate more of `s.values` and the page
 * genuinely draws different values. The tick count in this driver is
 * load-bearing and must not change.
 *
 * So jump the CLOCK instead and let the first of the existing ticks see a huge
 * dt: the advance lands on its target in one step. Same ticks, same reads,
 * settled picture. Harmless on the pre-refactor tree, which has no slide and
 * whose read cursor is driven by tick COUNT, not by the clock.
 */
function preSettleClock(clockRef) {
    clockRef.t += 1000;
}

export function frames() {
    const out = [];

    for (const [layoutName, layout] of [["movy", LAYOUT_MOVY], ["list", LAYOUT_LIST]]) {
        const clockRef = { t: 1000 };
        const store = makeStore();
        const ctl = makeController(clockRef, store);
        ctl.setLayout(layout);
        ctl.load({ prefix: "synth" });
        for (let i = 0; i < 60; i++) { clockRef.t += 18; ctl.tick(); }

        /* Warm every page: each kind reads its own CONTENT on arrival (the
         * items list, the preset name, the knob values), so a cold pass would
         * be recording how far the read rotation happened to have got. */
        for (let i = 0; i < ctl.state.pages.length; i++) {
            ctl.goToPage(i);
            for (let n = 0; n < 12; n++) { clockRef.t += 18; ctl.tick(); }
        }

        for (let i = 0; i < ctl.state.pages.length; i++) {
            /*
             * `remember: false`, AND IT IS THE DIFFERENCE BETWEEN COVERING FOUR
             * PAGES AND COVERING ALL OF THEM.
             *
             * goToPage restores the SECTION by default, so the six requests
             * 0..5 landed on 0, 1, 1, 1, 4, 5 -- pages 2 and 3 were never
             * rendered at all, and 42 scenario lines held 16 distinct frames.
             * What that missed is the worst thing here to miss: 1/2/3 are one
             * level`s three knob pages, i.e. a bank-bar GROUP spanning three
             * positions, and drawBankBar(ctx, index, ...) is exactly what this
             * refactor rethreaded. Only position 1 of it was recorded.
             *
             * Naming the frames after the requested index papered over that --
             * it fixed the collision and left the coverage hole reading as
             * coverage. Land on the page instead, and assert it.
             */
            ctl.goToPage(i, { remember: false });
            preSettleClock(clockRef);
            for (let n = 0; n < 6; n++) { clockRef.t += 18; ctl.tick(); }
            const j = ctl.state.pageIndex;
            if (j !== i) throw new Error(`page ${i} did not land: got ${j}`);
            const pg = ctl.state.pages[j];

            const fb = createFramebuffer();
            ctl.render(drawContext(fb), { title: TITLE, footer: FOOTER });
            out.push({
                name: `${layoutName}/${j}/${pg.kind}/plain`,
                frame: digest(fb),
            });

            /* No footer supplied: the body grows into the band. A separate
             * geometry, and one the refactor threads `chrome` past. */
            const fbNF = createFramebuffer();
            ctl.render(drawContext(fbNF), { title: TITLE });
            out.push({
                name: `${layoutName}/${j}/${pg.kind}/nofooter`,
                frame: digest(fbNF),
            });

            /*
             * An ENTERED door. Every door recorded above is inert, so the
             * entered arm of each kind -- a highlighted row and no brackets --
             * appeared in no frame of the baseline at all.
             */
            if (ctl.enterMenu()) {
                const fbE = createFramebuffer();
                ctl.render(drawContext(fbE), { title: TITLE, footer: FOOTER });
                out.push({
                    name: `${layoutName}/${j}/${pg.kind}/entered`,
                    frame: digest(fbE),
                });
                ctl.exitMenu();
            }

            /* The section picker: an overlay on the page set, drawn outside
             * drawPage and keeping its unconditional chrome. */
            ctl.openPicker();
            const fbP = createFramebuffer();
            ctl.render(drawContext(fbP), { title: TITLE, footer: FOOTER });
            out.push({
                name: `${layoutName}/${j}/${pg.kind}/picker`,
                frame: digest(fbP),
            });
            ctl.closePicker();
        }
    }

    /*
     * A HINT over a KNOB page.
     *
     * A FRESH controller per hint, because showHint latches: dismissHint sets
     * s.hintShown and every later showHint refuses. Driven through the same
     * settle-and-ask-where-you-landed dance as above.
     *
     * KNOB PAGES ONLY, and the omission is deliberate rather than an oversight.
     * The refactor CHANGES the hint over a MENU / ITEMS / PRESET page: the old
     * render() put the hint early-out above the per-kind dispatch, so a hint
     * over a menu page drew the KNOB GRID underneath it. Recording that here
     * would freeze the bug into the baseline. See the note at the hint site in
     * page_controller.mjs, and test_page_slide_composite.sh, which pins the new
     * behaviour instead.
     */
    for (const [layoutName, layout] of [["movy", LAYOUT_MOVY], ["list", LAYOUT_LIST]]) {
        /* One controller per page: showHint LATCHES (dismissHint sets
         * s.hintShown and every later showHint refuses), so the loop cannot
         * reuse one. Bounded by the page count, never by a literal -- an
         * `i < 8` cap here would silently stop recording past page 8. */
        for (let i = 0; ; i++) {
            const clockRef = { t: 1000 };
            const ctl = makeController(clockRef, makeStore());
            ctl.setLayout(layout);
            ctl.load({ prefix: "synth" });
            for (let n = 0; n < 60; n++) { clockRef.t += 18; ctl.tick(); }
            if (i >= ctl.state.pages.length) break;
            ctl.goToPage(i, { remember: false });
            preSettleClock(clockRef);
            for (let n = 0; n < 18; n++) { clockRef.t += 18; ctl.tick(); }
            const j = ctl.state.pageIndex;
            if (j !== i) throw new Error(`hint page ${i} did not land: got ${j}`);
            if (ctl.state.pages[j].kind !== "knobs") continue;
            ctl.showHint(["one", "two"], "H");
            const fb = createFramebuffer();
            ctl.render(drawContext(fb), { title: TITLE, footer: FOOTER });
            out.push({
                name: `${layoutName}/${j}/knobs/hint`,
                frame: digest(fb),
            });
        }
    }

    return out;
}

/* Run directly to emit a baseline. `name<TAB>sha256` per line. */
/* import.meta.url, not the basename of argv[1]: an `endsWith` on the name
 * fires for any other file that happens to be called page_frames.mjs. */
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
    for (const f of frames()) console.log(f.name + "\t" + f.frame);
}
