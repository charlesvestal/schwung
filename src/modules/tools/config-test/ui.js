/*
 * Config Test — throwaway harness for the per-module config system.
 *
 * Re-reads its own config.json on every tick and renders the current
 * values to the OLED. Lets us verify schwung-manager is writing to
 * the right places: change a value at move.local:7700/config/modules/
 * config-test and the screen updates within ~1s.
 */

const MODULE_DIR = "/data/UserData/schwung/modules/tools/config-test";
const CONFIG_PATH = MODULE_DIR + "/config.json";
const SECRET_PATH = MODULE_DIR + "/secrets/api_key.txt";

const SCREEN_W = 128;
const LINE_H = 8;

let lastReadFrame = -1;
let frameCount = 0;
let cfg = {};
let secretLen = 0;
let promptPreview = "";

function refreshConfig() {
    let raw = "";
    try { raw = host_read_file(CONFIG_PATH) || ""; } catch (e) { raw = ""; }
    if (raw) {
        try { cfg = JSON.parse(raw); } catch (e) { cfg = {}; }
    } else {
        cfg = {};
    }
    /* Secret: never read the value, just report whether it's set
     * and how long it is — same shape the host's "is_set" marker
     * uses, useful for proving the file landed at 0600 in the
     * right place. */
    let secretRaw = "";
    try { secretRaw = host_read_file(SECRET_PATH) || ""; } catch (e) { secretRaw = ""; }
    secretLen = secretRaw.length;

    /* Textarea preview: first ~32 chars so we can tell whether the
     * default_source resolver fired vs the saved override. */
    const sp = cfg.system_prompt;
    if (typeof sp === "string") {
        promptPreview = sp.replace(/\s+/g, " ").substring(0, 32);
    } else {
        promptPreview = "(unset)";
    }
}

function fmt(label, value) {
    return label + ": " + (value === undefined || value === null ? "—" : String(value));
}

globalThis.init = function() {
    frameCount = 0;
    refreshConfig();
};

globalThis.tick = function() {
    frameCount++;
    /* Re-read every ~1s. Cheap on a JSON the size of our schema. */
    if (frameCount - lastReadFrame > 44) {
        refreshConfig();
        lastReadFrame = frameCount;
    }

    clear_screen();
    print(2, 2, "Config Test", 1);
    /* divider */
    let y = 14;
    print(2, y,     fmt("enabled", cfg.enabled), 1); y += LINE_H;
    print(2, y,     fmt("mode",    cfg.mode),    1); y += LINE_H;
    print(2, y,     fmt("count/ratio", (cfg.count === undefined ? "—" : cfg.count) +
                                       " / " +
                                       (cfg.ratio === undefined ? "—" : cfg.ratio)), 1); y += LINE_H;
    print(2, y,     fmt("label", cfg.label_text), 1); y += LINE_H;
    print(2, y,     "prompt: " + promptPreview, 1); y += LINE_H;
    print(2, y,     "secret bytes: " + secretLen, 1);
};

globalThis.onMidiMessageInternal = function(data) {
    /* No interaction needed — module is read-only. */
};
