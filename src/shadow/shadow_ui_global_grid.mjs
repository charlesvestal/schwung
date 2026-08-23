/*
 * shadow_ui_global_grid.mjs — Global Settings, expressed as a module contract.
 *
 * Global Settings is not a module: it publishes no ui_hierarchy and its values
 * come from half a dozen different backends (shadow params, TTS, overlay
 * knobs, display mirror, feature flags). But everything the page engine needs
 * is a hierarchy plus chain_params, so the contract is synthesised here — the
 * same trick shadow_ui_slot_grid.mjs plays for a slot and for Master FX, and
 * the one buildSynthHierarchyFromChainParams plays for a component that
 * declares none of its own.
 *
 * Seven sections become seven levels, each planning to exactly ONE page. Six
 * are grids; Updates is a menu, the page kind that exists for entries with a
 * name, a consequence and nothing to show. One section, one page is what makes
 * sections-as-levels work at all: a section that spilled to two pages would
 * put the bank bar in charge of a split nobody chose, and the split would
 * arrive silently, so tests/host/test_global_settings_contract.sh pins the
 * per-section counts rather than trusting the shapes to stay put. Audio sits
 * at exactly eight.
 *
 * WHY NOT IN shadow_ui_slot_grid.mjs.
 *
 * That file holds TWO contracts on purpose, and warns that "Master FX getting
 * its own file is precisely how the two chain editors drifted apart in the
 * first place: one reasonable-sounding scope boundary at a time, until the
 * knob card worked on one screen and not the other." That warning is answered
 * here, not stepped around.
 *
 * The test it implies is SHARED SUBSTANCE, not shared topic. Those two live
 * together because they share the LFO pages outright — lfoParams / lfoLevels
 * is one declaration serving both — so splitting them would immediately
 * produce two copies of it, and every future LFO change would have to be made
 * twice or land on one screen only. Global Settings shares no page with
 * either: no LFO, no chain prefix, no preset actions, no per-slot storage
 * conventions, a wholly different accessor set. There is nothing here to
 * duplicate by separating it, so separating it cannot start the drift the
 * warning describes.
 *
 * The rule is the substance, not the filename. If Global Settings ever grows a
 * page shared with the slot contract, that page moves into the shared file.
 *
 * PURE. Hand it accessors and it tests with no UI, no device and no
 * framebuffer; nothing here reads a global, and the contract builds with no io
 * at all — a declaration that needs an accessor to exist has stopped being a
 * declaration. Accessor routing (which backend each key reads and writes, and
 * the persistence side effects that must ride along with a write) is NOT here;
 * it is the io built alongside this, the same division as createSlotGridIo.
 */

/*
 * Long options and short options are ONE mechanism with two renderings.
 *
 * The enum SQUARE sets three characters a line in the 5x3 font, so "Native"
 * comes out "Nat/ive" and reads as gibberish. Every other surface — the
 * held-knob header, the list row, the enum picker — has room for the real
 * word and exists to show it. So `options` stays long and `short_options`
 * serves only the square (render_page_movy.mjs consults it there and nowhere
 * else).
 *
 * Generalised, and this is the part worth carrying forward: any value too long
 * for a cell is a short_options entry, never a second code path. A case
 * short_options cannot express is a signal to extend the shared formatter —
 * not to branch on which surface is drawing.
 */
const OFF_ON = { options: ["Off", "On"], short_options: ["OFF", "ON"] };

/** A bool, which on the grid is an enum of two words drawn as a switch. */
function bool(key, name, dflt) {
    return Object.assign({ key, name, type: "enum", default: dflt }, OFF_ON);
}

/*
 * The stored value behind each enum INDEX, where the two differ.
 *
 * Transcribed from the `values` field of GLOBAL_SETTINGS_SECTIONS, and kept
 * even though nothing in this file consults it, because the information is
 * impossible to recover and silent to get wrong: resample_bridge stores 0 and
 * **2**, so an index-is-value assumption writes 1 — a mode that does not
 * exist — and the setting appears to do nothing. The io built on this contract
 * is the consumer; declaring the table here keeps it beside the options it
 * indexes rather than in a second list that can fall out of step.
 *
 * Enums absent from this table store their index directly.
 */
export const GLOBAL_ENUM_VALUES = {
    overlay_knobs: [0, 1, 2, 3],
    param_view: [0, 1],
    resample_bridge: [0, 2],
    skipback_shortcut: [0, 1],
    skipback_seconds: [30, 60, 120, 180, 240, 300],
    screen_reader_engine: ["espeak", "flite"],
    shadow_ui_trigger: [0, 1, 2],
};

/* ------------------------------------------------------------------ display */

export const DISPLAY_PARAMS = [
    bool("display_mirror", "Mirror", 0),
    { key: "overlay_knobs", name: "Overlay", type: "enum",
      options: ["+Shift", "+Jog Touch", "Off", "Native"],
      short_options: ["SHF", "JOG", "OFF", "NAT"], default: 0 },
    bool("pad_typing", "Pad Typ", 0),
    bool("text_preview", "Text Prv", 0),
    bool("midi_indicator_enabled", "MIDI Ch", 0),
    /* The grid is the default (tests/host/test_param_view_default.sh pins
     * paramViewGlobal = 1), so the default index here is Knobs. */
    { key: "param_view", name: "Params", type: "enum",
      options: ["List", "Knobs"], short_options: ["LST", "KNB"], default: 1 },
];

/* -------------------------------------------------------------------- audio */

export const AUDIO_PARAMS = [
    /* The arrow is ASCII and the 5x7 font draws it; the label is the direction
     * the audio travels, which is the whole content of the setting. */
    bool("link_audio_routing", "Move>Sch", 0),
    bool("link_audio_publish", "Sch>Link", 0),
    bool("latency_comp_enabled", "Latency", 0),
    /* Stored 0 or 2 — see GLOBAL_ENUM_VALUES. */
    { key: "resample_bridge", name: "Smp Src", type: "enum",
      options: ["Native", "Schwung Mix"], short_options: ["NAT", "MIX"], default: 0 },
    { key: "skipback_shortcut", name: "Skipback", type: "enum",
      options: ["Sh+Cap", "Sh+Vol+Cap"], short_options: ["S+C", "SVC"], default: 0 },
    /* Every option already fits the square, so there is no short form to
     * declare. short_options exists for the ones that do not fit; declaring it
     * where it is not needed is a second list to keep in step for nothing. */
    { key: "skipback_seconds", name: "Skip Len", type: "enum",
      options: ["30s", "1m", "2m", "3m", "4m", "5m"], default: 0 },
    bool("browser_preview", "Brws Prv", 1),
    /*
     * usbc_out_persist needs no exception, and this is why.
     *
     * It renders as "On (Main Out)" — a bool annotated with the source last
     * seen on the wire — because Move's own Settings screen keeps reading
     * "Mic" after Schwung restores the value, so this row is the only honest
     * read of what is actually routed. A three-character square cannot show
     * that.
     *
     * short_options is exactly that mechanism, and it already ships. So the
     * annotation is the LONG option and "ON" is the short one: one
     * declaration, two renderings, no per-surface branch. The square shows the
     * bool, which is the part the user sets; the annotation appears everywhere
     * with room for it.
     *
     * Both On indexes store 1. The wire source is read-only — Move's menu
     * still chooses it — so the io reads back whichever On index matches the
     * source and collapses both to "1" on write. That belongs to the io, not
     * to the declaration.
     */
    { key: "usbc_out_persist", name: "USB-C", type: "enum",
      options: ["Off", "On (Mic)", "On (Main Out)"],
      short_options: ["OFF", "ON", "ON"], default: 1 },
];

/* ------------------------------------------------------------ accessibility */

export const ACCESSIBILITY_PARAMS = [
    bool("screen_reader_enabled", "Reader", 0),
    { key: "screen_reader_engine", name: "Engine", type: "enum",
      options: ["eSpeak-NG", "Flite"], short_options: ["ESP", "FLI"], default: 0 },
    { key: "screen_reader_speed", name: "Speed", type: "float",
      min: 0.5, max: 6.0, step: 0.1, default: 1.0, unit: "x" },
    { key: "screen_reader_pitch", name: "Pitch", type: "int",
      min: 80, max: 180, step: 5, default: 110, unit: "Hz" },
    /* max 100 with unit "%" reads the raw value and appends the sign — the
     * x100 scaling in param_format only applies to a 0..1 fraction. */
    { key: "screen_reader_volume", name: "Voice Vol", type: "int",
      min: 0, max: 100, step: 5, default: 70, unit: "%" },
    { key: "screen_reader_debounce", name: "Debounce", type: "int",
      min: 0, max: 1000, step: 50, default: 300, unit: "ms" },
];

/* ---------------------------------------------- set pages / shortcuts / svc */

export const SET_PAGES_PARAMS = [
    bool("set_pages_enabled", "Set Pages", 0),
];

export const SHORTCUTS_PARAMS = [
    { key: "shadow_ui_trigger", name: "Trigger", type: "enum",
      options: ["Long Press", "Shift+Vol", "Both"],
      short_options: ["LNG", "S+V", "BTH"], default: 2 },
];

export const SERVICES_PARAMS = [
    bool("filebrowser_enabled", "Files", 0),
    bool("auto_update_check", "Auto Chk", 1),
    /* Opt-in, default off — see docs/plans on analytics. */
    bool("analytics_enabled", "Analytics", 0),
];

/* ------------------------------------------------------------------ updates */

/*
 * Detection runs on-device (catalog scan + version compare) so users can see
 * what is outdated without opening a browser. The INSTALL always happens via
 * the web manager — the on-device install paths silently failed for users
 * without a current shim, so they were removed.
 *
 * Two entries with a name and a consequence and nothing to show, which is the
 * definition of a menu page. There is no value to draw, so rendering them as
 * knob cells would spend the whole widget band on four words.
 */
export const UPDATES_ACTIONS = [
    { label: "[Check Updates]", action: "check_updates" },
    { label: "[Module Store]", action: "module_store" },
];

/* --------------------------------------------------------------- assembly */

/**
 * The sections, in the order they are paged through. `id` is the level name,
 * `label` is what the header and the section picker show.
 */
export const GLOBAL_SECTIONS = [
    { id: "display", label: "Display", params: DISPLAY_PARAMS },
    { id: "audio", label: "Audio", params: AUDIO_PARAMS },
    { id: "accessibility", label: "Screen Reader", params: ACCESSIBILITY_PARAMS },
    { id: "set_pages", label: "Set Pages", params: SET_PAGES_PARAMS },
    { id: "shortcuts", label: "Shortcuts", params: SHORTCUTS_PARAMS },
    { id: "services", label: "Services", params: SERVICES_PARAMS },
    { id: "updates", label: "Updates", params: [], menu: UPDATES_ACTIONS },
];

/** Every declared param, across all six grid sections. */
export function allGlobalParams() {
    const out = [];
    for (const s of GLOBAL_SECTIONS) for (const p of s.params) out.push(p);
    return out;
}

/**
 * Global Settings as a ui_hierarchy plus chain_params.
 *
 * Root carries no params of its own — it is pure navigation, so it plans to no
 * page and the seven sections are the whole page set. Its nav entries are what
 * name each page: planPages prefers a nav entry's label over the level's own,
 * which is the label users already see.
 *
 * [Help...] rides along as an ACTION entry on root rather than as a level.
 * It is a navigation entry into the existing help stack — a screen outside
 * this contract — so it must not become an eighth page, and an entry with no
 * `key` and no `level` is exactly what the planner walks past. Declaring it
 * here rather than dropping it keeps the section list whole for the host that
 * dispatches it; declaring it as a level would put a blank page between
 * Updates and the end.
 *
 * @param {object} [io]  unused by the declaration; accepted so the call shape
 *   matches createSlotGridIo's and so a future runtime-shaped section (one
 *   whose entries are only known at build time) has somewhere to read from
 *   without every call site changing.
 * @returns {{hierarchy: object, chainParams: Array}}
 */
export function buildGlobalSettingsContract(io) {
    const levels = {
        root: {
            label: "Settings",
            knobs: [],
            params: GLOBAL_SECTIONS
                .map((s) => ({ level: s.id, label: s.label }))
                .concat([{ action: "help", label: "[Help...]" }]),
        },
    };

    for (const s of GLOBAL_SECTIONS) {
        const level = {
            label: s.label,
            knobs: s.params.map((p) => p.key),
            params: s.params.map((p) => ({ key: p.key })),
        };
        if (s.menu) {
            level.menu = s.menu.map((m) => ({ label: m.label, action: m.action }));
            level.menu_label = s.label;
        }
        levels[s.id] = level;
    }

    return { hierarchy: { modes: null, levels }, chainParams: allGlobalParams() };
}
