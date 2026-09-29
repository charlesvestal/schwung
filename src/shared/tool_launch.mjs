/*
 * Which of a tool's launch paths runs, decided in ONE place.
 *
 * `standalone` was read two ways: the shadow UI took it only TOP-LEVEL
 * (`json.standalone`), while module_manager.c's text matcher found the key
 * anywhere in the file, so `capabilities.standalone` worked in one host and was
 * silently ignored by the other -- the one that actually runs on the device.
 * Both spellings are accepted now; top-level stays canonical.
 *
 * And `standalone` was tested LAST, after every `tool_config` branch, so a
 * module declaring it beside `tool_config.interactive` (or `skip_file_browser`,
 * or a file-browser config) was launched as an ordinary tool instead and the
 * declaration did nothing, with no log line. It is the more specific intent --
 * it replaces the host -- so it wins over tool_config. Overtake is decided
 * before this (a different component_type, scanned separately) and is not
 * affected. Surfaced by dbxhost (legsmechanical), which had to ship a separate
 * launcher module to get around it.
 *
 * Pure so tests/host can run it under node.
 */

export function declaresStandalone(json) {
    if (!json || typeof json !== "object") return false;
    if (json.standalone === true) return true;
    const caps = json.capabilities;
    return !!(caps && typeof caps === "object" && caps.standalone === true);
}

/* Returns one of: "standalone", "set_picker", "interactive", "file_browser",
 * "unavailable". `tool` is the scanned entry (standalone already resolved). */
export function toolLaunchKind(tool) {
    if (!tool) return "unavailable";
    if (tool.standalone) return "standalone";
    const tc = tool.tool_config;
    if (tc && tc.set_picker) return "set_picker";
    if (tc && tc.skip_file_browser && tc.interactive) return "interactive";
    if (tc && (tc.command || tc.interactive || tc.engines)) return "file_browser";
    return "unavailable";
}
