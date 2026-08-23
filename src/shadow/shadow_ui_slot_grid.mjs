/*
 * shadow_ui_slot_grid.mjs — settings, expressed as a module contract.
 *
 * A slot is not a module: it publishes no ui_hierarchy and its params do not
 * share one prefix. But everything the knob grid needs is a hierarchy plus
 * chain_params, so the contract is synthesised here — the same trick
 * buildSynthHierarchyFromChainParams already plays for a component that has
 * none of its own.
 *
 * There are TWO such contracts now — a slot's Settings position and Master
 * FX's — and they live in ONE file on purpose. They share the LFO pages
 * outright (see lfoParams / lfoLevels) and differ only in their values page,
 * their actions and their key prefix. Master FX getting its own file is
 * precisely how the two chain editors drifted apart in the first place: one
 * reasonable-sounding scope boundary at a time, until the knob card worked on
 * one screen and not the other. §1b of the Master FX variable-length design
 * says so at length.
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
     * everything the slot is listening to, so it declares nothing.
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

/** Actions, in the order they appear on the menu page. */
export const SLOT_GRID_ACTIONS = [
    { label: "Knob Mapping", action: "knobs", always: true },
    /* LFO 1 and LFO 2 are PAGES now, not menu entries — eight of their nine
     * params are turnable and the widgets draw the thing itself. */
    { label: "Save", action: "save", always: true },
    /* Save As stays even with nothing saved: it goes straight to the keyboard
     * where Save offers a generated name. Only DELETE is meaningless. Same
     * filter getChainSettingsItems applies to the list. */
    { label: "Save As", action: "save_as", always: true },
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
    Object.assign(levels, lfoLevels([1, 2]));
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

/* ======================================================================== *
 * MASTER FX SETTINGS — the same contract, one bus over.
 * ======================================================================== */

/*
 * Every master-bus key is addressed at IPC slot 0 under "master_fx:" — a
 * CONVENTION, not instrument slot 0 — so the prefix is declared once and every
 * key below carries it. That means the io needs no mapping table at all: the
 * declared key IS the real key, and the only thing between the two is the grid
 * stripping its own page prefix back off.
 */
export const MASTER_KEY_PREFIX = "master_fx:";

/*
 * The Master FX listen channel, as an ENUM with two representations.
 *
 * The wire (`master_fx:midi_channel`, shadow_config.json, and the shim's
 * master_fx_midi_channel) carries the REAL channel: -1 for All, 0..15 for MIDI
 * channels 1..16, matching the status byte's low nibble so the filter can
 * compare without arithmetic. An enum cell, though, is addressed by OPTION
 * INDEX — 0..16 here. The two are off by one and disagree about All, so the
 * conversion is pinned to the io boundary below rather than left for each call
 * site to remember.
 */
export const MFX_MIDI_CHANNEL_KEY = MASTER_KEY_PREFIX + "midi_channel";
export const MFX_MIDI_CHANNEL_ALL_WIRE = -1;
export const MFX_MIDI_CHANNEL_OPTIONS = ["All"].concat(
    Array.from({ length: 16 }, (_, i) => String(i + 1)));

/** Wire value (-1 / 0..15) -> option index (0..16). Anything else is All. */
export function mfxMidiChannelToIndex(wire) {
    const ch = parseInt(wire, 10);
    return (Number.isFinite(ch) && ch >= 0 && ch <= 15) ? ch + 1 : 0;
}

/** Option index (0..16) -> wire value. Index 0 is All. */
export function mfxMidiChannelFromIndex(index) {
    const i = parseInt(index, 10);
    return (Number.isFinite(i) && i >= 1 && i <= 16) ? i - 1 : MFX_MIDI_CHANNEL_ALL_WIRE;
}

/*
 * ONE value, and that is the whole page.
 *
 * The grid draws fewer than eight cells whenever a page has fewer (see
 * branchage in the render snapshots — a sparse page marks its unused positions
 * rather than leaving them blank), so a single knob here is a deliberate page,
 * not a broken one.
 *
 * IT USED TO BE TWO, AND THE FIRST ONE WAS INERT. `master_fx:volume` had no
 * handler anywhere: shadow_chain_mgmt.c serves `slot:volume` but has no master
 * case, so a key with no `fxN:` prefix fell through to mfx_slot 0 / param_key
 * "volume" and was written into whatever module happened to be loaded in
 * Master FX slot 1, as THAT module's own volume. The shim's
 * shim_handle_param_special does not serve it either, so the read came back
 * empty and the list's `if (!val) return "100%"` fallback drew a confident
 * fake 100%. A row that reads a lie and writes somewhere else is worse than no
 * row, so it is gone rather than wired up — the master bus level is Move's own
 * volume knob, which already reaches it.
 *
 * MIDI Ch below is the real one: the shim reads master_fx_midi_channel every
 * frame. Do not remove it by symmetry.
 */
export const MASTER_GRID_PARAMS = [
    /* Divable like every other enum that declares options, which is what makes
     * 17 choices usable from a knob at all. */
    { key: MFX_MIDI_CHANNEL_KEY, name: "MIDI Ch", type: "enum",
      options: MFX_MIDI_CHANNEL_OPTIONS, default: 0 },
];

/** Actions, in the order they appear on the menu page. */
export const MASTER_GRID_ACTIONS = [
    /* The list spells this "[Save MFX Preset]"; a menu page is already titled
     * Actions and sits under an "MFX >" header, so the brackets and the
     * restatement are noise. The ACTION KEY is unchanged — it is what
     * handleMasterFxSettingsAction dispatches on. */
    { label: "Save", action: "save", always: true },
    /*
     * Save As stays even with nothing saved, and here it is load-bearing: the
     * master bus has no Knob Mapping entry, so filtering it left a ONE ENTRY
     * menu page — a page you must enter in order to press a single button.
     * Same filter getMasterFxSettingsItems applies to the list.
     */
    { label: "Save As", action: "save_as", always: true },
    { label: "Delete", action: "delete", always: false },
];

/**
 * Page order is Master, LFO 1, LFO 2, Actions — the slot's order with the
 * values page shorter and Knob Mapping absent (the master bus has no knob
 * mapping table; §6 of the variable-length design records that as the one
 * genuinely easier thing about it).
 *
 * @param {boolean} hasPreset  whether a master preset is currently loaded
 */
export function masterGridHierarchy(hasPreset) {
    const menu = MASTER_GRID_ACTIONS
        .filter((a) => a.always || hasPreset)
        .map((a) => ({ label: a.label, action: a.action }));
    const levels = {
        root: {
            label: "Master",
            knobs: MASTER_GRID_PARAMS.map((p) => p.key),
            params: MASTER_GRID_PARAMS.map((p) => ({ key: p.key }))
                .concat([{ level: "lfo1", label: "LFO 1" },
                         { level: "lfo2", label: "LFO 2" },
                         { level: "actions", label: "Actions" }]),
        },
    };
    /* The SAME builder the slot contract uses, one bus over. */
    Object.assign(levels, lfoLevels([1, 2], MASTER_KEY_PREFIX));
    levels.actions = { label: "Actions", knobs: [], params: [], menu: menu, menu_label: "Actions" };
    return { modes: null, levels };
}

/** Every declared param across the root page and both LFO pages. */
export function allMasterGridParams() {
    return MASTER_GRID_PARAMS
        .concat(lfoParams(1, MASTER_KEY_PREFIX))
        .concat(lfoParams(2, MASTER_KEY_PREFIX));
}

const MASTER_LFO_KEY = new RegExp("^" + MASTER_KEY_PREFIX + "lfo[12]:");
const MASTER_LFO_TARGET = new RegExp("^" + MASTER_KEY_PREFIX + "lfo([12]):target$");

/**
 * The param accessors the grid drives the master bus through.
 *
 * Far thinner than createSlotGridIo, and for a reason worth stating: a slot
 * stores its settings under three different conventions and derives a fourth,
 * while every master-bus key is already spelled the way the shim serves it. So
 * there is no mapping here — only the contract, the modulation answer and the
 * LFO-target formatter, all three of which the slot version needs for exactly
 * the same reasons.
 *
 * @param {object} io
 * @param {(key:string)=>string}       io.readParam   read a REAL key at slot 0
 * @param {(key:string,v:string)=>any} io.writeParam  write a REAL key at slot 0
 * @param {()=>boolean}                io.hasPreset
 * @param {(action:string)=>any}       [io.runAction]  perform a menu action.
 *   Carried on the io rather than reached through the host's generic
 *   runSlotAction, which takes the IPC SLOT — and Master FX's IPC slot is 0,
 *   so "save" from here would have saved instrument slot 1's patch.
 * @param {(lfoIndex:number)=>object}  [io.describeTarget]  see createSlotGridIo
 * @param {(realKey:string)=>boolean}  [io.isModulated]
 */
export function createMasterGridIo(io) {
    const bare = (fullKey) => String(fullKey || "").replace(/^[^:]*:/, "");

    return {
        getParam(fullKey) {
            const k = bare(fullKey);
            if (k === "ui_hierarchy") return JSON.stringify(masterGridHierarchy(!!io.hasPreset()));
            if (k === "chain_params") return JSON.stringify(allMasterGridParams());
            /* Wire -> option index. A failed read must stay a failed read: the
             * grid distinguishes null from "", and turning either into index 0
             * would silently assert "All" for a channel we never saw. */
            if (k === MFX_MIDI_CHANNEL_KEY) {
                const raw = io.readParam(k);
                if (raw === null || raw === "") return raw;
                return String(mfxMidiChannelToIndex(raw));
            }
            return io.readParam(k);
        },

        /*
         * Only an LFO param can be modulated — the other LFO can drive it.
         * The listen channel is not a modulation target, and letting the host's
         * generic oracle answer for it would cost up to three IPC round trips
         * per tick to conclude "no", then wrongly conclude "yes": an unserved
         * `<key>:base` reads back as "" rather than null, which compares
         * unequal to the live value and wears the modulation tilde. That is the
         * bug createSlotGridIo.isModulated exists to prevent, and it would
         * arrive here identically.
         */
        isModulated(fullKey) {
            const k = bare(fullKey);
            if (!MASTER_LFO_KEY.test(k)) return false;
            if (!io.isModulated) return false;
            return !!io.isModulated(k);
        },

        formatValue(fullKey, raw, surface) {
            const m = MASTER_LFO_TARGET.exec(bare(fullKey));
            if (!m || !io.describeTarget) return null;
            const d = io.describeTarget(parseInt(m[1], 10) - 1);
            if (!d) return null;
            return surface === "header" ? d.long : d.short;
        },

        setParam(fullKey, value) {
            const k = bare(fullKey);
            /* Option index -> wire, the inverse of getParam's mapping. */
            if (k === MFX_MIDI_CHANNEL_KEY) {
                io.writeParam(k, String(mfxMidiChannelFromIndex(value)));
                return;
            }
            io.writeParam(k, String(value));
        },

        runAction(action) {
            if (io.runAction) return io.runAction(action);
        },
    };
}
