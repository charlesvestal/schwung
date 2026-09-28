/*
 * lfo_page.mjs — an LFO, expressed as one page of a module contract.
 *
 * Schwung's slot LFOs ("lfoN:<key>") and the Master FX LFOs
 * ("master_fx:lfoN:<key>") are the same params, the same waveform viz
 * group and the same one-rate-cell visibility rule, so Slot Settings and
 * Master FX Settings build their LFO pages from the builders below
 * (shadow_ui_slot_grid.mjs re-exports them).
 *
 * In shared/param_pages/ rather than beside those contracts because the page
 * is useful to anything that edits the same LFOs through this library — a
 * tool hosting its own copy of a chain draws the SAME page Schwung does by
 * merging these levels into its hierarchy, instead of re-declaring the
 * params and drifting from them. Pure: no imports, no globals.
 */

/*
 * The two slot LFOs, each its own page.
 *
 * They earn a grid rather than a menu: every one of the eight things on the
 * page is turnable, and the widgets say more than a list of words can. The
 * row-wide waveform draws the actual shape, depth and phase; Rate keeps a cell
 * of its own so its number (or its division) stays readable.
 *
 * Stored under "lfoN:<key>" — see makeSlotLfoCtx, which uses the same prefix.
 *
 * Enum words are <= 3 characters for the enum square, as everywhere else, via
 * the parallel short_options lists below.
 */
/*
 * Full option words, and a parallel short list for the enum SQUARE.
 *
 * The square is three characters a line; the held-knob header has room for the
 * real word and exists to show it. Declaring only the short form made the
 * header read "BI" and "THR", which tells you nothing you could not already
 * see in the cell.
 */
export const LFO_SHAPES = ["Sine", "Triangle", "Saw", "Square", "S&H", "Swishy"];
export const LFO_SHAPES_SHORT = ["SIN", "TRI", "SAW", "SQR", "S&H", "SWY"];
export const LFO_DIVISIONS = [
    "16 bar", "15 bar", "14 bar", "13 bar", "12 bar", "11 bar", "10 bar", "9 bar",
    "8 bar", "7 bar", "6 bar", "5 bar", "4 bar", "3 bar", "2 bar",
    "1/1", "1/1T", "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T",
    "1/16", "1/16T", "1/32", "1/32T",
];
export const LFO_DIVISIONS_SHORT = [
    "16b", "15b", "14b", "13b", "12b", "11b", "10b", "9b",
    "8b", "7b", "6b", "5b", "4b", "3b", "2b",
    "1/1", "1T", "1/2", "2T", "1/4", "4T", "1/8", "8T",
    "16", "16T", "32", "32T",
];

/**
 * ONE builder, two contracts.
 *
 * A slot's LFOs are stored as "lfoN:<key>" and Master FX's as
 * "master_fx:lfoN:<key>"; everything else about them — the params, the
 * ordering the viz group depends on, the one-rate-cell visibility condition —
 * is identical. So what is parameterised is the key prefix and the two facts
 * that genuinely differ, as arguments rather than as a second copy.
 *
 * A SECOND COPY OF THIS FUNCTION IS HOW THE TWO EDITORS DRIFTED. Master FX went
 * years without a windowed diagram, then shipped without the knob card, because
 * each feature landed in a Master-FX-shaped copy of something the slot chain
 * already had — or did not land at all. Anything added below therefore arrives
 * on both screens by construction.
 *
 * @param {number} lfoIndex   1-based
 * @param {string} [keyPrefix]  "" for a slot, "master_fx:" for the master bus.
 *   It prefixes the param keys AND the visibility condition, which is the part
 *   that is easy to miss: normalizeVisibilityConditionKey passes any key
 *   containing ":" straight through unchanged, so an unprefixed "lfo1:sync"
 *   would be resolved against slot 0's chain rather than the master bus, read
 *   empty, compare false, and hide BOTH rate cells rather than one.
 * @param {object} [opts]
 * @param {boolean} [opts.retrigger]  whether this LFO has a retrigger key.
 *   Defaults to "it is a slot's": the master bus has no notes to retrigger on
 *   and the shim serves no such key.
 * @param {{options:string[], short_options?:string[]}} [opts.targets]  the
 *   routings Target can be turned through, as the HOST resolved them — see
 *   lfoTargetParam. Absent, Target is a door and the host's picker sets it.
 */
export function lfoParams(lfoIndex, keyPrefix = "", opts = {}) {
    /* The viz group is a name scoped to ONE page, and an LFO is exactly one
     * page, so it does not need the prefix and stays "lfoN" for both. */
    const g = `lfo${lfoIndex}`;
    const k = (name) => `${keyPrefix}lfo${lfoIndex}:${name}`;
    const retrigger = opts.retrigger !== undefined ? !!opts.retrigger : !keyPrefix;
    /*
     * ROW 1 — what the modulator IS: where it goes, which way it swings, what
     * its clock is, whether a note restarts it.
     *
     * ROW 2 — what its motion LOOKS like. Shape, Depth and Phase are one `lfo`
     * viz group, so those three cells draw the actual waveform at its actual
     * depth and phase offset. The group is a hard adjacency gate — its
     * spanning roles must land contiguously on ONE row — which is why the
     * ordering below is not arbitrary.
     *
     * Rate is the LAST cell and outside the span. Inside it, a synced rate was
     * a density in a drawing and nothing else: "1/4" and "1/8T" read the same
     * at a glance. As its own cell it shows the number or the division, and
     * it still lends the wave its density through a span:false role (see
     * viz_draw's lfoRateFrac, which draws a division at the rate it plays).
     *
     * There is no Enabled cell. Choosing a target IS switching the LFO on, and
     * None is switching it off — the host writes `enabled` with the routing,
     * which is what pays for Retrigger's cell.
     *
     * Mode and Sync do not peek: two short words that flip on the next detent
     * are already fully shown by their square.
     */
    const params = [
        lfoTargetParam(k("target"), opts.targets),
        /*
         * span:false — it lends the graphic its baseline without joining the
         * cells the wave is drawn across. Counting it in the span would
         * straddle the row boundary and the graphic would not draw at all.
         */
        { key: k("polarity"), name: "Mode", type: "enum",
          options: ["Unipolar", "Bipolar"], short_options: ["UNI", "BI"], peek: false,
          viz: { group: g, role: "polarity", span: false } },
        { key: k("sync"), name: "Sync", type: "enum",
          options: ["Free", "Sync"], short_options: ["FRE", "SYN"], peek: false },
    ];
    if (retrigger) {
        params.push({ key: k("retrigger"), name: "Retrig", type: "enum",
                      options: ["Off", "On"], short_options: ["OFF", "ON"] });
    }
    const rates = [
        /*
         * ONE rate cell, not two. Free-run and synced rates are the same
         * control wearing different units, and showing both spends a cell on
         * whichever one the DSP is currently ignoring.
         *
         * The condition key carries its own "lfoN:" prefix deliberately:
         * normalizeVisibilityConditionKey passes any key containing ":"
         * straight through, so it resolves against the LFO instead of being
         * prefixed with the component. page_controller watches conditionKeys
         * and re-plans when one changes, so the cell swaps as you turn Sync.
         *
         * Both carry role "rate": only ever one of them is on the page, so the
         * group finds exactly one either way.
         */
        { key: k("rate_hz"), name: "Rate", type: "float", min: 0.1, max: 20, step: 0.1, unit: "Hz",
          visible_if: { param: k("sync"), equals: "0" },
          viz: { group: g, role: "rate", span: false } },
        { key: k("rate_div"), name: "Rate", type: "enum",
          options: LFO_DIVISIONS, short_options: LFO_DIVISIONS_SHORT,
          visible_if: { param: k("sync"), equals: "1" },
          viz: { group: g, role: "rate", span: false } },
    ];
    /*
     * With no Retrigger (the master bus) the page has seven cells, and the
     * wave's three cannot share row 1 with Target, Mode and Sync. Rate closes
     * row 1 instead, so the wave still owns row 2 whole rather than being
     * pushed across the row break, where it would not draw.
     */
    if (!retrigger) params.push(...rates);
    params.push(
        { key: k("shape"), name: "Shape", type: "enum",
          options: LFO_SHAPES, short_options: LFO_SHAPES_SHORT,
          viz: { group: g, role: "shape" } },
        /* Percent, not a raw fraction: "65%" is the value, "0.65" is the storage.
         * Bipolar is kept — a negative depth INVERTS the modulation, which is a
         * real feature, so the range reads -100%..+100% rather than 0..100%. */
        { key: k("depth"), name: "Depth", type: "float", min: -1, max: 1, step: 0.01, unit: "%",
          default: 1,
          viz: { group: g, role: "depth" } },
        { key: k("phase_offset"), name: "Phase", type: "float", min: 0, max: 1, step: 0.0417, unit: "%",
          viz: { group: g, role: "phase" } },
    );
    if (retrigger) params.push(...rates);
    return params;
}

/**
 * Target, as the knob sees it.
 *
 * A routing is TWO stored keys (`target` names a component, `target_param` one
 * of its params) and what is loaded decides which pairs exist, so no static
 * declaration can list them. The host that can resolve them hands them in as
 * one flat enum — None first — and translates the index at its io boundary:
 * the read answers the stored pair's position, the write commits the pair and
 * `enabled` together.
 *
 * `commit: "release"`: the square and the peek follow the turn, the routing is
 * written where the hand stops. Written per detent, scrolling from Cutoff to
 * Resonance would drive every parameter in between for a frame each.
 *
 * Without options it stays what it was: a `string` door the host's two-step
 * picker sets, opaque to a knob and showing the current target in the cell.
 */
export function lfoTargetParam(key, targets) {
    if (!targets || !Array.isArray(targets.options) || targets.options.length < 1) {
        return { key, name: "Targ", type: "string" };
    }
    const out = { key, name: "Targ", type: "enum", options: targets.options.slice(),
                  commit: "release" };
    if (Array.isArray(targets.short_options)) out.short_options = targets.short_options.slice();
    return out;
}

/*
 * The three-letter tag a routing wears in the enum square, by component KEY —
 * the square has three characters a line, so "FX 1: Freeverb" cannot go there
 * and the key is what distinguishes two of the same module anyway.
 */
function targetTag(compKey) {
    const k = String(compKey || "");
    let m;
    if (k === "synth") return "SYN";
    if (k === "buses") return "SND";
    if ((m = /^midi_fx(\d+)$/.exec(k))) return "MF" + m[1];
    if ((m = /^fx(\d+)$/.exec(k))) return "FX" + m[1];
    if ((m = /^lfo(\d+)$/.exec(k))) return "LF" + m[1];
    return k.slice(0, 3).toUpperCase();
}

const prettify = (key) => String(key || "").replace(/[_:]+/g, " ").trim()
    .split(/\s+/).filter(Boolean)
    .map((w) => w.charAt(0).toUpperCase() + w.slice(1)).join(" ");

/**
 * Every routing an LFO can be turned to, as the flat enum lfoTargetParam
 * declares — None first, then each component's params in the order offered.
 *
 * Pure: the host resolves what is loaded (that costs IPC and wants caching)
 * and hands it in; this only decides how the list reads, so every host
 * spells a routing the same way.
 *
 *   options        "Freeverb: Room Size" — the peek, the list, the header
 *   short_options  "FX1 Room Size" — the square keeps the first three
 *                  characters of each word: FX1 / ROO
 *   routes         [{target, param}] by the same index; routes[0] is None
 *
 * A stored routing the list does not offer (a param its module stopped
 * declaring, a component since unloaded) is APPENDED rather than read as
 * None: the LFO still drives it, and a knob that showed None would, on its
 * first detent, silently replace a routing nobody chose to remove.
 *
 * @param {object} a
 * @param {{key:string,label:string}[]} a.components  "__clear__" is skipped
 * @param {(compKey:string)=>{key:string,label:string}[]} a.paramsFor
 * @param {{target?:string, param?:string}} [a.current]
 */
export function lfoTargetOptions({ components, paramsFor, current } = {}) {
    const options = ["None"], short_options = ["NONE"], routes = [{ target: "", param: "" }];
    const nameOf = (label) => {
        const at = String(label || "").indexOf(": ");
        return at < 0 ? String(label || "") : String(label).slice(at + 2);
    };
    const add = (target, param, moduleName, paramLabel) => {
        options.push(moduleName + ": " + paramLabel);
        short_options.push(targetTag(target) + " " + paramLabel);
        routes.push({ target, param });
    };
    for (const c of (components || [])) {
        if (!c || !c.key || c.key === "__clear__") continue;
        for (const p of ((paramsFor && paramsFor(c.key)) || [])) {
            if (p && p.key) add(c.key, p.key, nameOf(c.label), p.label || prettify(p.key));
        }
    }
    const t = current && current.target ? String(current.target) : "";
    const pk = current && current.param ? String(current.param) : "";
    if (t && pk && lfoTargetIndex(routes, t, pk) < 0) {
        const comp = (components || []).find((c) => c && c.key === t);
        add(t, pk, comp ? nameOf(comp.label) : prettify(t), prettify(pk));
    }
    return { options, short_options, routes };
}

/** Index of a stored routing in `routes`, 0 (None) for none, -1 if absent. */
export function lfoTargetIndex(routes, target, param) {
    if (!target || !param) return 0;
    for (let i = 1; i < (routes || []).length; i++) {
        if (routes[i].target === target && routes[i].param === param) return i;
    }
    return -1;
}

/**
 * The keys on an LFO's page, in cell order. Eight on a slot (one of the two
 * rates is always hidden), seven on the master bus, which has no Retrigger.
 */
export function lfoKnobKeys(lfoIndex, keyPrefix = "", opts = {}) {
    return lfoParams(lfoIndex, keyPrefix, opts).map((p) => p.key);
}

/**
 * The LFO LEVELS, ready to merge into a hierarchy — the second half of the
 * sharing, and the half that is easy to forget.
 *
 * lfoParams alone is not enough: `visible_if` has to travel on the LEVEL param
 * entry, because that is what isHiddenParam reads. A condition declared only in
 * chain_params is never consulted when planning which keys get a knob, so a
 * contract that copied the params but assembled its own level would show BOTH
 * rate cells and push a real param off the page. That is exactly the kind of
 * near-miss a second copy produces, so the assembly is shared too.
 *
 * @param {number[]} indices    which LFOs, 1-based
 * @param {string} [keyPrefix]  see lfoParams
 * @param {object} [opts]  see lfoParams; only `retrigger` changes the keys
 * @returns {object} { lfo1: {...}, lfo2: {...} } keyed by LEVEL name, which is
 *   unprefixed — a level name is internal to the hierarchy, not a param key.
 */
export function lfoLevels(indices, keyPrefix = "", opts = {}) {
    const levels = {};
    for (const n of indices) {
        levels["lfo" + n] = {
            label: "LFO " + n,
            knobs: lfoKnobKeys(n, keyPrefix, opts),
            params: lfoParams(n, keyPrefix, opts).map((p) => (
                p.visible_if ? { key: p.key, visible_if: p.visible_if } : { key: p.key }
            )),
        };
    }
    return levels;
}
