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
    { key: "volume", name: "Volume", type: "float", min: 0, max: 4, step: 0.05, default: 1 },
    { key: "muted", name: "Mute", type: "enum", options: ["OFF", "ON"] },
    { key: "soloed", name: "Solo", type: "enum", options: ["OFF", "ON"] },
    { key: "transpose", name: "Trsp", type: "int", min: -12, max: 12, step: 1 },
    { key: "receive_channel", name: "Recv", type: "enum", options: ["ALL"].concat(CHANNELS) },
    { key: "forward_channel", name: "Fwd", type: "enum", options: ["THR", "AUT"].concat(CHANNELS) },
    { key: "midi_fx_pre_mode", name: "MFX Pre", type: "enum", options: ["OFF", "ON"] },
    { key: "mpe_mode", name: "MPE", type: "enum", options: ["OFF", "ON"] },
];

/** Actions, in the order they appear on the menu page. */
export const SLOT_GRID_ACTIONS = [
    { label: "Knob Mapping", action: "knobs", always: true },
    { label: "LFO 1", action: "lfo1", always: true },
    { label: "LFO 2", action: "lfo2", always: true },
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
    return {
        modes: null,
        levels: {
            root: {
                label: "Slot",
                knobs: SLOT_GRID_PARAMS.map((p) => p.key),
                params: SLOT_GRID_PARAMS.map((p) => ({ key: p.key })),
                menu: menu,
                menu_label: "Actions",
            },
        },
    };
}

/** Which real param key a grid key reads and writes, or null when derived. */
export function realKeyFor(gridKey) {
    if (gridKey === "mpe_mode") return null;            /* derived, see below */
    if (gridKey === "midi_fx_pre_mode") return "midi_fx_pre_mode";  /* bare */
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
 */
export function createSlotGridIo(io) {
    const bare = (fullKey) => String(fullKey || "").replace(/^[^:]*:/, "");

    return {
        getParam(fullKey) {
            const k = bare(fullKey);
            if (k === "ui_hierarchy") return JSON.stringify(slotGridHierarchy(!!io.hasPreset()));
            if (k === "chain_params") return JSON.stringify(SLOT_GRID_PARAMS);
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
