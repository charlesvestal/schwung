const MODULE_DIR = "/data/UserData/schwung/modules/tools/move-mcp";
const CONFIG_PATH = MODULE_DIR + "/config.json";
const SECRET_PATH = MODULE_DIR + "/secrets/access_token.txt";
const LINE_H = 8;

const CC_BACK = 51;
const CC_JOG_CLICK = 3;

let frame = 0;
let cfg = {};
let tokenSet = false;
let status = "Config only";

const defaults = {
    enabled: false,
    bind_mode: "lan",
    require_token: true,
    allow_read: true,
    allow_write: false,
    allow_delete: false,
    allow_actions: false,
    max_upload_mb: 64,
    pack_root: "/data/UserData/schwung/move-mpc/packs",
    log_requests: true
};

function refreshConfig() {
    let raw = "";
    try { raw = host_read_file(CONFIG_PATH) || ""; } catch (e) { raw = ""; }

    let saved = {};
    if (raw) {
        try { saved = JSON.parse(raw); } catch (e) { saved = {}; }
    }

    cfg = Object.assign({}, defaults, saved);

    let secret = "";
    try { secret = host_read_file(SECRET_PATH) || ""; } catch (e) { secret = ""; }
    tokenSet = secret.trim().length > 0;

    if (!cfg.enabled) {
        status = "Disabled";
    } else if (cfg.require_token && !tokenSet) {
        status = "Needs token";
    } else if (cfg.allow_write || cfg.allow_delete || cfg.allow_actions) {
        status = "Write capable";
    } else {
        status = "Read only";
    }
}

function yes(v) {
    return v ? "yes" : "no";
}

function drawScreen() {
    clear_screen();
    print(0, 0, "Move MCP", 1);
    draw_line(0, 10, 127, 10, 1);

    let y = 14;
    print(0, y, "Status: " + status, 1); y += LINE_H;
    print(0, y, "URL: move.local:7700", 1); y += LINE_H;
    print(0, y, "Bind: " + cfg.bind_mode, 1); y += LINE_H;
    print(0, y, "Read: " + yes(cfg.allow_read) + " Write: " + yes(cfg.allow_write), 1); y += LINE_H;
    print(0, y, "Delete: " + yes(cfg.allow_delete) + " Act: " + yes(cfg.allow_actions), 1); y += LINE_H;
    print(0, y, "Token: " + (cfg.require_token ? (tokenSet ? "set" : "missing") : "off"), 1);

    print(0, 56, "Click: refresh Back: exit", 1);
}

globalThis.init = function() {
    frame = 0;
    refreshConfig();
};

globalThis.tick = function() {
    frame++;
    if (frame % 44 === 0) {
        refreshConfig();
    }
    drawScreen();
};

globalThis.onMidiMessageInternal = function(data) {
    const statusByte = data[0] & 0xF0;
    if (statusByte !== 0xB0) return;

    const cc = data[1];
    const value = data[2];
    if (value === 0) return;

    if (cc === CC_JOG_CLICK) {
        refreshConfig();
    } else if (cc === CC_BACK && typeof host_exit_module === "function") {
        host_exit_module();
    }
};

globalThis.onMidiMessageExternal = function(_data) {};
