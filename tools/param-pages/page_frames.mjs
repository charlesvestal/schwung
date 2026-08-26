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

export const CHAIN_PARAMS = KEYS.map((k, i) => ({
    key: k, name: "P" + i, type: "float", min: 0, max: 1, step: 0.01,
}));

export const HIER = {
    modes: null,
    levels: {
        root: {
            label: "T",
            knobs: KEYS,
            params: CHAIN_PARAMS.map((p) => ({ key: p.key })),
            /* A MENU page. */
            menu: [{ level: "stuff", label: "Stuff" }],
            /* A PRESET page. */
            list_param: "preset",
            count_param: "preset_count",
            name_param: "preset_name",
        },
        /* An ITEMS page. */
        stuff: { label: "Stuff", items_param: "thing_list", select_param: "thing_index" },
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
    store.preset = "0";
    store.preset_count = "12";
    store.preset_name = "Fat Bass";
    store.thing_list = JSON.stringify(["Alpha", "Beta", "Gamma"]);
    store.thing_index = "1";
    return store;
}

export function makeController(clockRef, store) {
    return createController({
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
            /* goToPage restores the SECTION, so it can land on a different
             * page of it than the index handed over. Ask where we landed. */
            ctl.goToPage(i);
            for (let n = 0; n < 6; n++) { clockRef.t += 18; ctl.tick(); }
            const j = ctl.state.pageIndex;
            const pg = ctl.state.pages[j];
            /* The name carries the REQUESTED index as well as the one we
             * landed on: goToPage restores the section, so several requests
             * resolve to the same page and a name built from `j` alone
             * collides. A colliding name silently shrinks the baseline. */

            const fb = createFramebuffer();
            ctl.render(drawContext(fb), { title: TITLE, footer: FOOTER });
            out.push({
                name: `${layoutName}/req${i}/${j}/${pg.kind}/plain`,
                frame: digest(fb),
            });

            /* No footer supplied: the body grows into the band. A separate
             * geometry, and one the refactor threads `chrome` past. */
            const fbNF = createFramebuffer();
            ctl.render(drawContext(fbNF), { title: TITLE });
            out.push({
                name: `${layoutName}/req${i}/${j}/${pg.kind}/nofooter`,
                frame: digest(fbNF),
            });

            /* The section picker: an overlay on the page set, drawn outside
             * drawPage and keeping its unconditional chrome. */
            ctl.openPicker();
            const fbP = createFramebuffer();
            ctl.render(drawContext(fbP), { title: TITLE, footer: FOOTER });
            out.push({
                name: `${layoutName}/req${i}/${j}/${pg.kind}/picker`,
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
        for (let i = 0; i < 8; i++) {
            const clockRef = { t: 1000 };
            const ctl = makeController(clockRef, makeStore());
            ctl.setLayout(layout);
            ctl.load({ prefix: "synth" });
            for (let n = 0; n < 60; n++) { clockRef.t += 18; ctl.tick(); }
            if (i >= ctl.state.pages.length) break;
            ctl.goToPage(i);
            for (let n = 0; n < 18; n++) { clockRef.t += 18; ctl.tick(); }
            const j = ctl.state.pageIndex;
            if (ctl.state.pages[j].kind !== "knobs") continue;
            ctl.showHint(["one", "two"], "H");
            const fb = createFramebuffer();
            ctl.render(drawContext(fb), { title: TITLE, footer: FOOTER });
            out.push({
                name: `${layoutName}/req${i}/${j}/knobs/hint`,
                frame: digest(fb),
            });
        }
    }

    return out;
}

/* Run directly to emit a baseline. `name<TAB>sha256` per line. */
if (process.argv[1] && process.argv[1].endsWith("page_frames.mjs")) {
    for (const f of frames()) console.log(f.name + "\t" + f.frame);
}
