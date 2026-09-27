/*
 * lfo_page.mjs — an LFO, expressed as one page of a module contract.
 *
 * Schwung's slot LFOs ("lfoN:<key>") and the Master FX LFOs
 * ("master_fx:lfoN:<key>") are the same nine params, the same waveform viz
 * group and the same one-rate-cell visibility rule, so Slot Settings and
 * Master FX Settings build their LFO pages from the builders below
 * (shadow_ui_slot_grid.mjs re-exports them).
 *
 * In shared/param_pages/ rather than beside those contracts because the page
 * is useful to anything that edits the same LFOs through this library — a
 * tool hosting its own copy of a chain draws the SAME page Schwung does by
 * merging these levels into its hierarchy, instead of re-declaring nine
 * params and drifting from them. Pure: no imports, no globals.
 */

/*
 * The two slot LFOs, each its own page.
 *
 * They earn a grid rather than a menu: eight of the nine things an LFO has are
 * turnable, and the widgets say more than a list of words can. The shape cell
 * draws the actual waveform, Enabled draws as a switch, Depth and Rate as
 * knobs. Only Target is a door.
 *
 * Stored under "lfoN:<key>" — see makeSlotLfoCtx, which uses the same prefix.
 *
 * Enum words are <= 3 characters for the enum square, as everywhere else.
 * rate_div is the one that does not fit that rule: its vocabulary is "16bar",
 * "1/16T", "1/32T", 5 and 6 characters, which the square breaks across two
 * lines badly. It is declared as a param but NOT as a knob, so it lands on an
 * overflow page where the cell is the same size but at least it is not
 * competing for one of the eight. A proper fix is a wider widget for it, or
 * short and long option labels.
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
 * "master_fx:lfoN:<key>"; everything else about them — the nine params, the
 * ordering the viz group depends on, the one-rate-cell visibility condition —
 * is identical. So the only thing parameterised is the key prefix.
 *
 * A SECOND COPY OF THIS FUNCTION IS HOW THE TWO EDITORS DRIFTED. Master FX went
 * years without a windowed diagram, then shipped without the knob card, because
 * each feature landed in a Master-FX-shaped copy of something the slot chain
 * already had — or did not land at all. Anything added below therefore arrives
 * on both screens by construction. If a future difference is genuinely needed,
 * it belongs as another argument here, not as a fork of the file.
 *
 * @param {number} lfoIndex   1-based
 * @param {string} [keyPrefix]  "" for a slot, "master_fx:" for the master bus.
 *   It prefixes the param keys AND the visibility condition, which is the part
 *   that is easy to miss: normalizeVisibilityConditionKey passes any key
 *   containing ":" straight through unchanged, so an unprefixed "lfo1:sync"
 *   would be resolved against slot 0's chain rather than the master bus, read
 *   empty, compare false, and hide BOTH rate cells rather than one.
 */
export function lfoParams(lfoIndex, keyPrefix = "") {
    /* The viz group is a name scoped to ONE page, and an LFO is exactly one
     * page, so it does not need the prefix and stays "lfoN" for both. */
    const g = `lfo${lfoIndex}`;
    const k = (name) => `${keyPrefix}lfo${lfoIndex}:${name}`;
    return [
        /*
         * ROW 1 — what the modulator IS: where it goes, whether it runs, how it
         * is scaled, what its clock is.
         *
         * ROW 2 — what its motion LOOKS like: shape, rate, depth, phase. Those
         * four are declared as one `lfo` viz group, so instead of four separate
         * cells the whole second row draws the actual waveform, at its actual
         * depth, with its actual phase offset. The group is a hard adjacency
         * gate — its roles must land contiguously on ONE row — which is exactly
         * why the ordering below is not arbitrary.
         *
         * Declared rather than detected: the detector wants rate and depth to
         * share a stem, and "rate_hz" against "depth" does not match, so it
         * never fired. A declared group wins over detection anyway.
         */
        /* A door: clicking opens the existing two-step target picker. Declared
         * `string` so it is opaque (a knob cannot turn it) and divable, and so
         * the cell shows the current target rather than a blank frame. */
        { key: k("target"), name: "Targ", type: "string" },
        { key: k("enabled"), name: "On", type: "enum",
          options: ["Off", "On"], short_options: ["OFF", "ON"] },
        /*
         * span:false — it lends the graphic its baseline without joining the
         * four cells the wave is drawn across. Counting it in the span would
         * straddle the row boundary and the graphic would not draw at all.
         */
        { key: k("polarity"), name: "Mode", type: "enum",
          options: ["Unipolar", "Bipolar"], short_options: ["UNI", "BI"],
          viz: { group: g, role: "polarity", span: false } },
        { key: k("sync"), name: "Sync", type: "enum",
          options: ["Free", "Sync"], short_options: ["FRE", "SYN"] },

        { key: k("shape"), name: "Shape", type: "enum",
          options: LFO_SHAPES, short_options: LFO_SHAPES_SHORT,
          viz: { group: g, role: "shape" } },
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
          viz: { group: g, role: "rate" } },
        { key: k("rate_div"), name: "Rate", type: "enum",
          options: LFO_DIVISIONS, short_options: LFO_DIVISIONS_SHORT,
          visible_if: { param: k("sync"), equals: "1" },
          viz: { group: g, role: "rate" } },
        /* Percent, not a raw fraction: "65%" is the value, "0.65" is the storage.
         * Bipolar is kept — a negative depth INVERTS the modulation, which is a
         * real feature, so the range reads -100%..+100% rather than 0..100%. */
        { key: k("depth"), name: "Depth", type: "float", min: -1, max: 1, step: 0.01, unit: "%",
          default: 1,
          viz: { group: g, role: "depth" } },
        { key: k("phase_offset"), name: "Phase", type: "float", min: 0, max: 1, step: 0.0417, unit: "%",
          viz: { group: g, role: "phase" } },
    ];
}

/**
 * Nine declared, one of them always hidden — so EIGHT show and an LFO is
 * exactly one page.
 *
 * The two rates never appear together (visible_if on sync), and that is what
 * pays for Phase. Retrigger is the one still missing: ten params do not fit
 * eight cells however they are arranged, and of the two, phase offset is the
 * one a per-slot LFO reaches for more often. Retrigger stays editable in the
 * list view.
 */
export function lfoKnobKeys(lfoIndex, keyPrefix = "") {
    return lfoParams(lfoIndex, keyPrefix).map((p) => p.key);
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
 * @returns {object} { lfo1: {...}, lfo2: {...} } keyed by LEVEL name, which is
 *   unprefixed — a level name is internal to the hierarchy, not a param key.
 */
export function lfoLevels(indices, keyPrefix = "") {
    const levels = {};
    for (const n of indices) {
        levels["lfo" + n] = {
            label: "LFO " + n,
            knobs: lfoKnobKeys(n, keyPrefix),
            params: lfoParams(n, keyPrefix).map((p) => (
                p.visible_if ? { key: p.key, visible_if: p.visible_if } : { key: p.key }
            )),
        };
    }
    return levels;
}
