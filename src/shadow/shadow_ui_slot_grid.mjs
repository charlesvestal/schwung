/*
 * shadow_ui_slot_grid.mjs — slot settings, expressed as a module contract.
 *
 * A slot is not a module: it publishes no ui_hierarchy and its params do not
 * share one prefix. But everything the knob grid needs is a hierarchy plus
 * chain_params, so the contract is synthesised here — the same trick
 * buildSynthHierarchyFromChainParams already plays for a component that has
 * none of its own.
 *
 * Its own file, not another thousand lines of shadow_ui.js, because all of it
 * is pure: hand it four accessors and it can be tested with no UI, no device
 * and no framebuffer. The host supplies the accessors; nothing here reads a
 * global.
 *
 * The eight values fill one page exactly. The actions become a menu page, the
 * page kind that exists for entries with a name, a consequence and nothing to
 * show.
 */

/** 1..16 as strings, for the channel enums. */
const CHANNELS = (() => {
    const out = [];
    for (let i = 1; i <= 16; i++) out.push(String(i));
    return out;
})();

/*
 * Fwd Ch is stored from -2 upward (-2 THRU, -1 AUTO, 0..15 = channels 1..16)
 * but the grid drives an enum by INDEX from 0, so every crossing is offset.
 * Getting this wrong loses the whole negative half of the range rather than
 * failing loudly, which is why it is a named constant with a test of its own.
 */
export const FWD_OFFSET = 2;

/*
 * Every int becomes an ENUM with words. That is the real gain over the list,
 * which shows Recv Ch as 0 and Fwd Ch as -2 and expects you to know that 0
 * means All and -2 means THRU.
 *
 * Option words are at most three characters because the enum SQUARE sets two
 * lines of the 5x3 font: "THRU" comes out "THR/U" and "POST" comes out
 * "POS/T". The cost is that the held-knob header, which has room for more,
 * also shows THR and AUT. One options list serving both a 3-char cell and a
 * roomy header is a real limitation — the fix is separate short and long
 * labels, not longer words here.
 *
 * MIDI FX Pre is on/off rather than a word pair for exactly that reason: it
 * draws as a switch like Mute and Solo, and the header reads "MFX PRE  ON"
 * instead of an unreadable three-letter abbreviation.
 *
 * Row 1 is what you touch while playing, row 2 is what you set once.
 */
export const SLOT_GRID_PARAMS = [
    /* Volume is a GAIN of 0..4, not a 0..1 fraction, so `unit: "%"` alone leaves it
     * reading "1.00%". An explicit format scales it: 1.0 gain reads 100%, which is
     * what the list has always shown. */
    /* Max 2.0 = 200% = +6 dB. Must match the list editor's own slot:volume
     * range in shadow_ui.js, or the same setting has two ceilings. */
    { key: "volume", name: "Volume", type: "float", min: 0, max: 2, step: 0.05, default: 1,
      display_format: ".0%" },
    { key: "muted", name: "Mute", type: "enum", options: ["Off", "On"], short_options: ["OFF", "ON"],
      default: 0 },
    { key: "soloed", name: "Solo", type: "enum", options: ["Off", "On"], short_options: ["OFF", "ON"],
      default: 0 },
    { key: "transpose", name: "Trsp", type: "int", min: -12, max: 12, step: 1, default: 0 },
    /*
     * Recv is the one setting here with NO honest default.
     *
     * Every other value has an obvious neutral — off, unity, no transposition,
     * Auto — but a slot's receive channel defaults to the slot's OWN number,
     * which this contract cannot know: it is synthesised once, not per slot.
     * "All" would be a guess, and a wrong guess here silently re-routes
     * everything the slot is listening to. Declaring nothing means a
     * double-tap does nothing, which is the right answer to a question with no
     * answer.
     */
    { key: "receive_channel", name: "Recv", type: "enum",
      options: ["All"].concat(CHANNELS.map((c) => "Ch " + c)),
      short_options: ["ALL"].concat(CHANNELS) },
    /* Index 1 is Auto — stored -1, see FWD_OFFSET — which is the documented
     * default for a slot that has not been told otherwise. */
    { key: "forward_channel", name: "Fwd", type: "enum",
      options: ["Thru", "Auto"].concat(CHANNELS.map((c) => "Ch " + c)),
      short_options: ["THR", "AUT"].concat(CHANNELS), default: 1 },
    { key: "midi_fx_pre_mode", name: "MFX Pre", type: "enum",
      options: ["Off", "On"], short_options: ["OFF", "ON"], default: 0 },
    { key: "mpe_mode", name: "MPE", type: "enum", options: ["Off", "On"], short_options: ["OFF", "ON"],
      default: 0 },
];

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

export function lfoParams(lfoIndex) {
    const g = `lfo${lfoIndex}`;
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
        { key: `lfo${lfoIndex}:target`, name: "Targ", type: "string" },
        { key: `lfo${lfoIndex}:enabled`, name: "On", type: "enum",
          options: ["Off", "On"], short_options: ["OFF", "ON"] },
        /*
         * span:false — it lends the graphic its baseline without joining the
         * four cells the wave is drawn across. Counting it in the span would
         * straddle the row boundary and the graphic would not draw at all.
         */
        { key: `lfo${lfoIndex}:polarity`, name: "Mode", type: "enum",
          options: ["Unipolar", "Bipolar"], short_options: ["UNI", "BI"],
          viz: { group: g, role: "polarity", span: false } },
        { key: `lfo${lfoIndex}:sync`, name: "Sync", type: "enum",
          options: ["Free", "Sync"], short_options: ["FRE", "SYN"] },

        { key: `lfo${lfoIndex}:shape`, name: "Shape", type: "enum",
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
        { key: `lfo${lfoIndex}:rate_hz`, name: "Rate", type: "float", min: 0.1, max: 20, step: 0.1, unit: "Hz",
          visible_if: { param: `lfo${lfoIndex}:sync`, equals: "0" },
          viz: { group: g, role: "rate" } },
        { key: `lfo${lfoIndex}:rate_div`, name: "Rate", type: "enum",
          options: LFO_DIVISIONS, short_options: LFO_DIVISIONS_SHORT,
          visible_if: { param: `lfo${lfoIndex}:sync`, equals: "1" },
          viz: { group: g, role: "rate" } },
        /* Percent, not a raw fraction: "65%" is the value, "0.65" is the storage.
         * Bipolar is kept — a negative depth INVERTS the modulation, which is a
         * real feature, so the range reads -100%..+100% rather than 0..100%. */
        { key: `lfo${lfoIndex}:depth`, name: "Depth", type: "float", min: -1, max: 1, step: 0.01, unit: "%",
          default: 1,
          viz: { group: g, role: "depth" } },
        { key: `lfo${lfoIndex}:phase_offset`, name: "Phase", type: "float", min: 0, max: 1, step: 0.0417, unit: "%",
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
export function lfoKnobKeys(lfoIndex) {
    return lfoParams(lfoIndex).map((p) => p.key);
}

/** Actions, in the order they appear on the menu page. */
export const SLOT_GRID_ACTIONS = [
    { label: "Knob Mapping", action: "knobs", always: true },
    /* LFO 1 and LFO 2 are PAGES now, not menu entries — eight of their nine
     * params are turnable and the widgets draw the thing itself. */
    { label: "Save", action: "save", always: true },
    /* Save As and Delete mean nothing until a preset exists — the same filter
     * getChainSettingsItems applies to the list. */
    { label: "Save As", action: "save_as", always: false },
    { label: "Delete", action: "delete", always: false },
];

/**
 * @param {boolean} hasPreset  whether this slot already holds a saved preset
 */
export function slotGridHierarchy(hasPreset) {
    const menu = SLOT_GRID_ACTIONS
        .filter((a) => a.always || hasPreset)
        .map((a) => ({ label: a.label, action: a.action }));
    /*
     * Page order is Main, LFO 1, LFO 2, Actions.
     *
     * The menu therefore lives on its OWN level rather than on root: a level
     * emits its menu straight after its own grids, before any level it
     * navigates to, so a menu on root would land second — between the values
     * and the LFOs. Actions are what you do when you have finished, so they
     * belong at the end.
     */
    const levels = {
        root: {
            label: "Slot",
            knobs: SLOT_GRID_PARAMS.map((p) => p.key),
            params: SLOT_GRID_PARAMS.map((p) => ({ key: p.key }))
                .concat([{ level: "lfo1", label: "LFO 1" },
                         { level: "lfo2", label: "LFO 2" },
                         { level: "actions", label: "Actions" }]),
        },
    };
    for (const n of [1, 2]) {
        levels["lfo" + n] = {
            label: "LFO " + n,
            knobs: lfoKnobKeys(n),
            /* visible_if travels on the LEVEL param entry, which is what
             * isHiddenParam reads — a condition declared only in chain_params
             * is never consulted when planning which keys get a knob. */
            params: lfoParams(n).map((p) => (
                p.visible_if ? { key: p.key, visible_if: p.visible_if } : { key: p.key }
            )),
        };
    }
    levels.actions = { label: "Actions", knobs: [], params: [], menu: menu, menu_label: "Actions" };
    return { modes: null, levels };
}

/** Every declared param across the slot page and both LFO pages. */
export function allSlotGridParams() {
    return SLOT_GRID_PARAMS.concat(lfoParams(1)).concat(lfoParams(2));
}

/** Which real param key a grid key reads and writes, or null when derived. */
export function realKeyFor(gridKey) {
    if (gridKey === "mpe_mode") return null;            /* derived, see below */
    if (gridKey === "midi_fx_pre_mode") return "midi_fx_pre_mode";  /* bare */
    /* LFO params are declared with their real prefix already ("lfo1:shape"),
     * the same one makeSlotLfoCtx uses, so they pass straight through. Adding
     * "slot:" would address a param that does not exist and read empty. */
    if (/^lfo[12]:/.test(gridKey)) return gridKey;
    return "slot:" + gridKey;
}

/**
 * The param accessors the grid drives the slot through.
 *
 * The grid asks for "<prefix>:<key>" and this maps that onto what a slot
 * actually stores:
 *
 *   volume, muted, soloed, transpose, receive_channel  ->  "slot:<key>"
 *   midi_fx_pre_mode                                   ->  bare key
 *   forward_channel                                    ->  "slot:*", offset
 *   mpe_mode                                           ->  DERIVED
 *
 * mpe_mode is not stored anywhere: it is recv=All plus fwd=THRU plus the synth
 * flag, so it reads through isMpeMode and writes through setMpeMode, which is
 * the existing compound handler that knows how to save and restore the
 * pre-MPE channels. Treating it as an ordinary param would set one third of it.
 *
 * @param {object} io
 * @param {(key:string)=>string}   io.readSlotParam   read a REAL key
 * @param {(key:string,v:string)=>any} io.writeSlotParam  write a REAL key
 * @param {()=>boolean}            io.isMpeMode
 * @param {(on:boolean)=>void}     io.setMpeMode
 * @param {()=>boolean}            io.hasPreset
 * @param {(lfoIndex:number)=>object} [io.describeTarget]  resolve LFO N's
 *   routing to {short, header, long} — see shared/lfo_target_label.mjs. The
 *   host owns it because it costs IPC and therefore wants caching; omitted,
 *   the target simply reads as its stored key, which is what it did before.
 */
export function createSlotGridIo(io) {
    const bare = (fullKey) => String(fullKey || "").replace(/^[^:]*:/, "");

    return {
        getParam(fullKey) {
            const k = bare(fullKey);
            if (k === "ui_hierarchy") return JSON.stringify(slotGridHierarchy(!!io.hasPreset()));
            if (k === "chain_params") return JSON.stringify(allSlotGridParams());
            if (k === "mpe_mode") return io.isMpeMode() ? "1" : "0";
            if (k === "forward_channel") {
                const raw = parseInt(io.readSlotParam("slot:forward_channel"), 10);
                /* Default to AUTO (-1) rather than 0, which is channel 1: an
                 * unreadable value must not silently pin the slot to a channel. */
                const v = Number.isFinite(raw) ? raw : -1;
                return String(v + FWD_OFFSET);
            }
            const real = realKeyFor(k);
            return real ? io.readSlotParam(real) : "";
        },

        /*
         * Is this param being driven by a modulation source?
         *
         * Answered HERE rather than by the host's generic oracle, for two
         * reasons. It was wrong: that oracle falls back to comparing a live
         * value against `<key>:base`, and no slot-level key serves `:base` —
         * an unserved key answers "" rather than null, so every real `slot:*`
         * setting compared unequal and wore the modulation tilde. Volume,
         * Mute, Solo, Transpose, Recv and Fwd, all at once.
         *
         * And it was expensive: that fallback is up to THREE IPC round trips,
         * spent once per tick on a view whose whole design is one read per
         * tick — about 8ms of the frame, to answer a question whose answer is
         * fixed. A slot-level setting is not a modulation target; only the
         * LFO params are, since an LFO can drive the other one.
         */
        isModulated(fullKey) {
            const k = bare(fullKey);
            if (!/^lfo[12]:/.test(k)) return false;
            if (!io.isModulated) return false;
            /* The REAL key: the grid addresses these as "slot:lfo1:depth" and
             * the device knows them as "lfo1:depth". */
            return !!io.isModulated(realKeyFor(k));
        },

        /*
         * An LFO's target is the one value here that is a KEY, not a number or
         * a word from a declared list: it reads "fx1" and means "Room Size on
         * the Freeverb in FX 1". The cell has 30px and the header has 76, so
         * they get different halves of that rather than the same string cut
         * twice — the same split short_options makes for enums.
         */
        formatValue(fullKey, raw, surface) {
            const k = bare(fullKey);
            const m = /^lfo([12]):target$/.exec(k);
            if (!m || !io.describeTarget) return null;
            const d = io.describeTarget(parseInt(m[1], 10) - 1);
            if (!d) return null;
            return surface === "header" ? d.long : d.short;
        },

        setParam(fullKey, value) {
            const k = bare(fullKey);
            if (k === "mpe_mode") {
                const want = parseInt(value, 10) ? true : false;
                /* Only on a real change: the compound handler stashes the
                 * pre-MPE channels, so running it again would stash the MPE
                 * values as the thing to restore. */
                if (want !== !!io.isMpeMode()) io.setMpeMode(want);
                return;
            }
            if (k === "forward_channel") {
                const idx = parseInt(value, 10);
                const v = Number.isFinite(idx) ? idx : FWD_OFFSET;
                return io.writeSlotParam("slot:forward_channel", String(v - FWD_OFFSET));
            }
            const real = realKeyFor(k);
            if (real) io.writeSlotParam(real, String(value));
        },
    };
}
