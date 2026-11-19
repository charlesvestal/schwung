/*
 * web_settings_adopt.mjs — the audio settings shadow_config.json shares with
 * schwung-manager, and how to tell the manager's write from our own.
 *
 * TWO WRITERS, ONE FILE. The manager's Settings page writes these four keys
 * into shadow_config.json and does nothing else: it has no SHM path to the
 * shim for them. The shadow UI read them ONCE at boot into cached* vars, and
 * every saveMasterFxChainConfig() -- which the periodic autosave runs every
 * ~5 s whether or not anything changed -- wrote those caches back over the
 * file. So choosing "Schwung Mix" for Move Resample on the web page reverted
 * to Native a few seconds later, the page's own poll showed it reverting, and
 * the shim never saw the change at all.
 *
 * The fix is a three-way compare, not "re-read the file and believe it":
 * `persisted` is what the shadow UI last loaded or wrote. A file value that
 * differs from it was written by somebody else, and is ADOPTED (applied to the
 * shim and the cache). A file value equal to it is our own write, so a change
 * made on the device -- cache updated, file not yet saved -- still wins.
 * Comparing file against cache instead would revert every on-device change
 * at the next sync, which is this bug pointed the other way.
 *
 * Pure: no I/O, no globals. tests/host/test_web_settings_adopt.sh runs it.
 */

/* config key in shadow_config.json -> master_fx param the shim serves. */
export const WEB_AUDIO_SETTINGS = [
    { configKey: "resample_bridge_mode", param: "resample_bridge",      kind: "resample" },
    { configKey: "link_audio_routing",   param: "link_audio_routing",   kind: "bool" },
    { configKey: "link_audio_publish",   param: "link_audio_publish",   kind: "bool" },
    { configKey: "latency_comp_enabled", param: "latency_comp_enabled", kind: "bool" },
];

/* The manager writes booleans as JSON booleans; older files may carry 0/1 or
 * "true"/"false". All normalise to a real boolean. */
export function normaliseBool(raw) {
    if (raw === true || raw === false) return raw;
    if (typeof raw === "number") return raw !== 0;
    const t = String(raw).trim().toLowerCase();
    return t === "1" || t === "true" || t === "on";
}

/*
 * Which settings in `fileCfg` were changed by someone other than us.
 *
 *   fileCfg            parsed shadow_config.json
 *   persisted          { configKey: normalisedValue } last loaded/written by
 *                      the shadow UI; a missing key means "never seen", and a
 *                      file value is then adopted (it can only be external)
 *   parseResampleMode  shadow_ui.js's parseResampleBridgeMode, passed in so
 *                      the one parser the C side is tested against stays the
 *                      only one
 *
 * Returns [{ configKey, param, value }] with `value` normalised (a number for
 * resample, a boolean otherwise). A key absent from the file is never a
 * change: absence is not a request to reset.
 */
export function externalSettingChanges(fileCfg, persisted, parseResampleMode) {
    const out = [];
    if (!fileCfg || typeof fileCfg !== "object") return out;
    for (const s of WEB_AUDIO_SETTINGS) {
        if (!(s.configKey in fileCfg)) continue;
        const raw = fileCfg[s.configKey];
        if (raw === undefined || raw === null) continue;
        const value = s.kind === "resample" ? parseResampleMode(raw) : normaliseBool(raw);
        if (persisted && Object.prototype.hasOwnProperty.call(persisted, s.configKey) &&
            persisted[s.configKey] === value) continue;
        out.push({ configKey: s.configKey, param: s.param, value });
    }
    return out;
}

/* The wire value shadow_set_param takes for a normalised setting value. */
export function settingWireValue(value) {
    if (value === true) return "1";
    if (value === false) return "0";
    return String(value);
}
