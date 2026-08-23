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

/* ------------------------------------------------------------ accessor routing
 *
 * WHICH BACKEND EACH KEY READS AND WRITES, and what must ride along with the
 * write. Derived by reading every branch of getMasterFxSettingValue and
 * adjustMasterFxSetting in shadow_ui.js (both are MISNAMED — they serve Global
 * Settings, not Master FX) and transcribed here so it is one declaration rather
 * than a shape recovered from a 240-line if-chain.
 *
 * The hazard this table exists for: adjustMasterFxSetting is DELTA-BASED and
 * SIDE-EFFECTFUL. Six of its branches also call saveMasterFxChainConfig() and
 * set a module-level cache var. A converted absolute write that drops either
 * sets the param, looks correct on screen, and loses it on reboot — silently.
 * So persistence is declared per key here, PERSISTING_KEYS is DERIVED from it
 * (never a second hand-kept list), and writeGlobalParam applies it.
 *
 *   key                    | read backend            | write backend            | persist | cache var              | modal
 *   -----------------------+-------------------------+--------------------------+---------+------------------------+------
 *   display_mirror         | display_mirror_get      | display_mirror_set       | -       | -                      | -
 *   overlay_knobs          | overlay_knobs_get_mode  | overlay_knobs_set_mode   | SAVE    | -                      | -
 *   pad_typing             | padSelectGlobal         | setPadSelectGlobal       | own     | padSelectGlobal        | -
 *   text_preview           | textPreviewGlobal       | setTextPreviewGlobal     | own     | textPreviewGlobal      | -
 *   midi_indicator_enabled | midiIndicatorEnabled    | midi_indicator_set       | -       | midiIndicatorEnabled   | -
 *   param_view             | paramViewGlobal         | paramViewGlobal =        | own     | paramViewGlobal        | -
 *   link_audio_routing     | master_fx: param        | master_fx: param         | SAVE    | cachedLinkAudioRouting | warnIfLinkDisabled
 *   link_audio_publish     | master_fx: param        | master_fx: param         | SAVE    | cachedLinkAudioPublish | warnIfLinkDisabled
 *   latency_comp_enabled   | master_fx: param        | master_fx: param         | SAVE    | cachedLatencyCompEnabled | -
 *   resample_bridge        | master_fx: param        | master_fx: param         | SAVE    | cachedResampleBridgeMode | Schwung Mix (mode 2)
 *   skipback_shortcut      | skipback_shortcut_get   | skipback_shortcut_set    | -       | -                      | -
 *   skipback_seconds       | skipback_seconds_get    | skipback_seconds_set     | -       | -                      | -
 *   browser_preview        | previewEnabled          | previewEnabled =         | own     | previewEnabled         | -
 *   usbc_out_persist       | master_fx: param (+src) | master_fx: param         | SAVE    | cachedUsbcOutPersist   | -
 *   screen_reader_enabled  | tts_get_enabled         | tts_set_enabled          | -       | -                      | -
 *   screen_reader_engine   | tts_get_engine          | tts_set_engine           | -       | -                      | -
 *   screen_reader_speed    | tts_get_speed           | tts_set_speed            | -       | -                      | -
 *   screen_reader_pitch    | tts_get_pitch           | tts_set_pitch            | -       | -                      | -
 *   screen_reader_volume   | tts_get_volume          | tts_set_volume           | -       | -                      | -
 *   screen_reader_debounce | tts_get_debounce        | tts_set_debounce         | -       | -                      | -
 *   set_pages_enabled      | set_pages_get           | set_pages_set            | -       | -                      | -
 *   shadow_ui_trigger      | shadow_ui_trigger_get   | shadow_ui_trigger_set    | -       | -                      | -
 *   filebrowser_enabled    | filebrowserEnabled      | flag file + host_system_cmd | own  | filebrowserEnabled     | File Browser
 *   auto_update_check      | autoUpdateCheckEnabled  | autoUpdateCheckEnabled = | own     | autoUpdateCheckEnabled | -
 *   analytics_enabled      | host_get_analytics_enabled | host_set_analytics_enabled | -  | -                      | -
 *
 * Three kinds of persistence, and conflating them is how a write goes missing:
 *
 *   "save"  saveMasterFxChainConfig() — the SHARED sink. These are the keys
 *           writeGlobalParam persists via io.persist(), and only these.
 *   "own"   a key-specific writer (savePadTypingConfig, saveFilebrowserConfig,
 *           …). It is inseparable from the write itself, so it lives in the
 *           host's writeParam beside the assignment it saves, not here — the
 *           generic persist() must not fire for these or every key would look
 *           persisted and the assertion that proves persistence would be
 *           vacuous.
 *   null    the backend persists itself (tts_*, display_mirror_*, the feature
 *           flags), or the value is genuinely session-scoped.
 *
 * `modal` records the writes that raise an overlay. Those are the host's to
 * raise — the hand-off is Task 9 — but they are declared here because a write
 * whose modal is forgotten is the same class of silent loss as a missing save.
 *
 * BACKENDS ARE NAMED IN A DOTTED FORM (`tts.get_speed`, not `tts_get_speed`)
 * and that is deliberate, not cosmetic. This module is asserted PURE against
 * its own source with a regex that cannot tell an identifier from a string
 * literal, so spelling the accessors as identifiers here would trip the purity
 * check with a table that reads nothing. Dotted, they are unambiguously data —
 * and the check keeps its teeth for the case it is actually for.
 */
export const GLOBAL_ROUTING = {
    display_mirror:         { read: "display_mirror.get",     write: "display_mirror.set",     persist: null,   cache: null,                     modal: null },
    overlay_knobs:          { read: "overlay_knobs.get_mode", write: "overlay_knobs.set_mode", persist: "save", cache: null,                     modal: null },
    pad_typing:             { read: "js.padSelectGlobal",     write: "js.setPadSelectGlobal",  persist: "own",  cache: "padSelectGlobal",        modal: null },
    text_preview:           { read: "js.textPreviewGlobal",   write: "js.setTextPreviewGlobal",persist: "own",  cache: "textPreviewGlobal",      modal: null },
    midi_indicator_enabled: { read: "js.midiIndicatorEnabled",write: "midi_indicator.set",     persist: null,   cache: "midiIndicatorEnabled",   modal: null },
    param_view:             { read: "js.paramViewGlobal",     write: "js.paramViewGlobal",     persist: "own",  cache: "paramViewGlobal",        modal: null },

    link_audio_routing:     { read: "master_fx",              write: "master_fx",              persist: "save", cache: "cachedLinkAudioRouting",   modal: "link" },
    link_audio_publish:     { read: "master_fx",              write: "master_fx",              persist: "save", cache: "cachedLinkAudioPublish",   modal: "link" },
    latency_comp_enabled:   { read: "master_fx",              write: "master_fx",              persist: "save", cache: "cachedLatencyCompEnabled", modal: null },
    resample_bridge:        { read: "master_fx",              write: "master_fx",              persist: "save", cache: "cachedResampleBridgeMode", modal: "resample" },
    skipback_shortcut:      { read: "skipback_shortcut.get",  write: "skipback_shortcut.set",  persist: null,   cache: null,                     modal: null },
    skipback_seconds:       { read: "skipback_seconds.get",   write: "skipback_seconds.set",   persist: null,   cache: null,                     modal: null },
    browser_preview:        { read: "js.previewEnabled",      write: "js.previewEnabled",      persist: "own",  cache: "previewEnabled",         modal: null },
    usbc_out_persist:       { read: "master_fx",              write: "master_fx",              persist: "save", cache: "cachedUsbcOutPersist",   modal: null },

    screen_reader_enabled:  { read: "tts.get_enabled",        write: "tts.set_enabled",        persist: null,   cache: null,                     modal: null },
    screen_reader_engine:   { read: "tts.get_engine",         write: "tts.set_engine",         persist: null,   cache: null,                     modal: null },
    screen_reader_speed:    { read: "tts.get_speed",          write: "tts.set_speed",          persist: null,   cache: null,                     modal: null },
    screen_reader_pitch:    { read: "tts.get_pitch",          write: "tts.set_pitch",          persist: null,   cache: null,                     modal: null },
    screen_reader_volume:   { read: "tts.get_volume",         write: "tts.set_volume",         persist: null,   cache: null,                     modal: null },
    screen_reader_debounce: { read: "tts.get_debounce",       write: "tts.set_debounce",       persist: null,   cache: null,                     modal: null },

    set_pages_enabled:      { read: "set_pages.get",          write: "set_pages.set",          persist: null,   cache: null,                     modal: null },
    shadow_ui_trigger:      { read: "shadow_ui_trigger.get",  write: "shadow_ui_trigger.set",  persist: null,   cache: null,                     modal: null },

    filebrowser_enabled:    { read: "js.filebrowserEnabled",  write: "js.filebrowserEnabled",  persist: "own",  cache: "filebrowserEnabled",     modal: "filebrowser" },
    auto_update_check:      { read: "js.autoUpdateCheckEnabled", write: "js.autoUpdateCheckEnabled", persist: "own", cache: "autoUpdateCheckEnabled", modal: null },
    analytics_enabled:      { read: "host.get_analytics_enabled", write: "host.set_analytics_enabled", persist: null, cache: null,               modal: null },
};

/**
 * The keys whose write must be followed by saveMasterFxChainConfig().
 *
 * DERIVED from GLOBAL_ROUTING rather than listed, because a hand-kept second
 * list is exactly the drift this whole table exists to prevent: a key added to
 * one and not the other persists on screen and not on disk.
 */
export const PERSISTING_KEYS = new Set(
    Object.keys(GLOBAL_ROUTING).filter((k) => GLOBAL_ROUTING[k].persist === "save"));

/**
 * The STORED value for an engine value.
 *
 * The knob engine and the enum picker both work in INDEXES (`knob_engine.mjs`
 * clamps an enum to `0..options.length-1`). Most of these enums store their
 * index — but resample_bridge stores 0 and **2**, screen_reader_engine stores
 * "espeak"/"flite", and skipback_seconds stores 30..300. Writing the index
 * would set a mode that does not exist and the setting would appear to do
 * nothing.
 */
export function globalStoredValue(key, engineValue) {
    if (key === "usbc_out_persist") {
        /* Four options, two states: every On index stores 1. The extra three
         * carry only the wire annotation on the surfaces with room for it —
         * the source is read-only, Move's own menu still chooses it. */
        return (Number(engineValue) > 0) ? "1" : "0";
    }
    const values = GLOBAL_ENUM_VALUES[key];
    if (!values) return String(engineValue);
    let i = Math.round(Number(engineValue));
    if (!Number.isFinite(i)) i = 0;
    i = Math.max(0, Math.min(values.length - 1, i));
    return String(values[i]);
}

/**
 * The engine value for a STORED value — the inverse of globalStoredValue.
 *
 * A failed read is passed through untouched. `null` means the read did not
 * complete and `""` means the channel served us nothing; neither is an index,
 * and turning either into 0 would report "Native"/"Off" as fact. See the
 * three-answers rule in CLAUDE.md.
 */
export function globalEngineValue(key, stored) {
    if (stored === null || stored === undefined || stored === "") return stored;
    const values = GLOBAL_ENUM_VALUES[key];
    if (!values) return String(stored);
    /* Compare as strings: the stored side arrives off the wire as text, and the
     * tables hold numbers for every enum except screen_reader_engine. */
    const i = values.map(String).indexOf(String(stored));
    return String(i < 0 ? 0 : i);
}

/**
 * Read one Global Settings value, as the engine wants it.
 *
 * @param {{readParam:(key:string)=>string}} io
 */
export function readGlobalParam(io, key) {
    if (key === "usbc_out_persist") {
        /* A bool. The source is NOT a state here -- it decorates the On label,
         * which buildGlobalSettingsContract builds per entry. Reading it as an
         * index would make the annotation selectable, which is what the three-
         * and four-option spellings both got wrong. */
        const on = io.readParam("usbc_out_persist");
        if (on === null || on === undefined || on === "") return on;
        return String(on) === "1" ? "1" : "0";
    }
    return globalEngineValue(key, io.readParam(key));
}

/**
 * Write one Global Settings value, absolutely, with the persistence that must
 * ride along.
 *
 * @param {{writeParam:(key:string,v:string)=>any, persist?:()=>any}} io
 */
export function writeGlobalParam(io, key, value) {
    io.writeParam(key, globalStoredValue(key, value));
    if (PERSISTING_KEYS.has(key) && typeof io.persist === "function") io.persist();
}

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
    /* Gates BOTH the file browser WAV preview and the User Presets scroll
     * audition -- one "hear it before you pick it" switch, not one each. The
     * stored key stays browser_preview so existing choices survive; the label
     * says what it now covers. Default OFF: auditioning applies state to the
     * live slot, which is not something to do to someone who did not ask. */
    bool("browser_preview", "Audition", 0),
    /*
     * usbc_out_persist IS A BOOL. The parenthetical is a readout, not a choice.
     *
     * It is On or Off -- whether Schwung restores the USB-C out source at boot.
     * The "(Main Out)" suffix reports the source last seen on the wire, which
     * matters because Move's own Settings screen keeps reading "Mic" after
     * Schwung restores the value: this row is the only honest read of what is
     * actually routed.
     *
     * IT WAS BRIEFLY MODELLED AS THREE OPTIONS, THEN FOUR, AND BOTH WERE WRONG.
     * Putting the annotation in the option set turns one choice into three
     * indistinguishable "On"s you have to jog past, and implies you can pick
     * the source here. You cannot -- it is read-only and Move's own menu
     * chooses it. Reported from the device: "it should be on or off and the ()
     * shows the last saved value".
     *
     * So there are two options, and the ON LABEL IS BUILT PER ENTRY from the
     * observed source (annotateUsbcOption, applied in buildGlobalSettingsContract).
     * The contract is rebuilt every time Global Settings is opened, so the
     * readout is current without the declaration pretending to be a control.
     * With nothing observed the label is a plain "On" -- the old code omitted
     * the parenthetical in exactly that case, and naming a source that was
     * never seen would mislead the user who came here to check.
     */
    { key: "usbc_out_persist", name: "USB-C", type: "enum",
      options: ["Off", "On"], short_options: ["OFF", "ON"], default: 1 },
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
 * Entries with a name and a consequence and nothing to show, which is the
 * definition of a menu page. There is no value to draw, so rendering them as
 * knob cells would spend the whole widget band on four words.
 *
 * [HELP...] IS THE THIRD ONE, and it is here because there is nowhere else it
 * can be.
 *
 * It was declared as an action entry on `root` on the theory that "the host
 * dispatches it" — but root is pure navigation and plans to NO page, so the
 * planner walked past it and nothing ever drew it. There was no affordance:
 * the section picker enumerates PAGES, and an entry on a level that is not a
 * page is invisible from every surface. A declaration nothing can reach is the
 * same as a deleted one, only harder to notice.
 *
 * The two obvious repairs are both closed. A `help` LEVEL is an eighth page,
 * which sections-as-levels forbids and tests/host/test_global_settings_contract
 * .sh pins twice over (seven pages, and no page whose name matches /help/i). A
 * one-entry menu level is the shape the Master FX contract already records as a
 * mistake — "a ONE ENTRY actions menu, which the knob grid draws as a menu page
 * you have to enter to press a single button".
 *
 * So it joins the only menu this contract has. That is a demotion from the peer
 * of a section it used to be, and it is stated rather than hidden: Help now
 * lives one row below [Module Store] on the last page. The section LABEL stays
 * "Updates" because the page-name check above forbids the honest "Updates &
 * Help" — worth revisiting if that check is ever relaxed.
 */
export const UPDATES_ACTIONS = [
    { label: "[Check Updates]", action: "check_updates" },
    { label: "[Module Store]", action: "module_store" },
    { label: "[Help...]", action: "help" },
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
 * [Help...] is NOT here. It used to be an action entry on root, which reads
 * well and does nothing: root plans to no page, the planner walks past an entry
 * with no `key` and no `level`, and the section picker enumerates pages — so it
 * had no surface anywhere. It is an entry on the Updates menu now; see
 * UPDATES_ACTIONS for why that is the only place left for it.
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
            params: GLOBAL_SECTIONS.map((s) => ({ level: s.id, label: s.label })),
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

    return {
        hierarchy: { modes: null, levels },
        chainParams: annotateUsbcOption(allGlobalParams(), io),
    };
}

/**
 * Decorate usbc_out_persist's "On" with the source last seen on the wire.
 *
 * The parameter is a BOOL — On or Off, whether Schwung restores the USB-C out
 * source at boot. The "(Main Out)" suffix is a READOUT of a read-only value
 * that Move's own menu owns, and it belongs on the label rather than in the
 * option set: a user must not be able to jog "the source".
 *
 * Applied at contract-build time because createGlobalGridIo rebuilds the
 * contract on every entry — so the readout is current each time the screen is
 * opened, exactly as fresh as the old read-time formatter was, without the
 * declaration having to pretend the annotation is a state.
 *
 * Unobserved (-1) leaves a plain "On". Naming a source that nothing has seen
 * would mislead the one user who came here because Move's screen was lying.
 *
 * COPIES the entry rather than mutating it: GLOBAL_PARAMS is module-level and
 * shared across every entry, so writing through it would make one session's
 * annotation stick to the next.
 */
export function annotateUsbcOption(params, io) {
    if (!io || typeof io.readParam !== "function") return params;
    const src = io.readParam("usbc_out_source");
    const suffix = String(src) === "1" ? " (Main Out)"
                 : String(src) === "0" ? " (Mic)"
                 : "";
    if (!suffix) return params;
    return params.map((p) => (p && p.key === "usbc_out_persist")
        ? { ...p, options: [p.options[0], p.options[1] + suffix] }
        : p);
}

/**
 * The engine-facing io for Global Settings — the same shape createMasterGridIo
 * returns, built on the low-level accessors the host supplies.
 *
 * The split is the point. Everything that can be decided without a device is
 * decided here (which key is an enum, what its stored value is, which writes
 * must persist); the host half is only the concrete backends and the cache-var
 * assignments that cannot leave shadow_ui.js. There is exactly one routing
 * table and it is the one above.
 *
 * Nothing here is modulated and nothing here is an LFO target, so there is no
 * isModulated and no formatValue — unlike the two chain contracts, whose
 * versions exist to stop the host's generic modulation oracle spending three
 * IPC round trips per tick to answer "no" and then answering "yes" by mistake.
 *
 * @param {object} io
 * @param {(key:string)=>string}       io.readParam   raw stored value, or
 *   `null` for a read that did not complete and `""` for one the channel served
 *   with nothing. Both are passed straight through — see readGlobalParam.
 * @param {(key:string,v:string)=>any} io.writeParam  absolute write, including
 *   the key-specific side effects (cache var, own save fn, modal).
 * @param {()=>any}              [io.persist]   saveMasterFxChainConfig
 * @param {(action:string)=>any} [io.runAction] perform an Updates menu action
 */
export function createGlobalGridIo(io) {
    const bare = (fullKey) => String(fullKey || "").replace(/^[^:]*:/, "");
    const contract = buildGlobalSettingsContract(io);

    return {
        getParam(fullKey) {
            const k = bare(fullKey);
            if (k === "ui_hierarchy") return JSON.stringify(contract.hierarchy);
            if (k === "chain_params") return JSON.stringify(contract.chainParams);
            if (!GLOBAL_ROUTING[k]) return "";
            return readGlobalParam(io, k);
        },

        setParam(fullKey, value) {
            const k = bare(fullKey);
            if (!GLOBAL_ROUTING[k]) return;
            writeGlobalParam(io, k, value);
        },

        /* Not a modulation target and not an LFO target — answered flatly
         * rather than left to the host's generic oracle, which would compare
         * each key against an unserved `<key>:base`, read "" instead of null,
         * and hang the modulation tilde on every row. */
        isModulated() { return false; },

        runAction(action) {
            if (io.runAction) return io.runAction(action);
        },
    };
}
