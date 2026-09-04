#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The tool time estimate ("about N remaining").
#
# getToolProcessingRatio() exists TWICE — src/shadow/shadow_ui.js drives the
# confirm screen, src/shadow/shadow_ui_tools.mjs drives the processing screen —
# and both feed the same user-visible number. Drift between them shows up as an
# estimate that changes when the screen does.
#
# The failure this pins: neither copy read tool_config.processing_ratio, only
# the per-engine value. A module with no tool_config.engines[] therefore always
# got the hardcoded 0.5. That went unnoticed for the life of the field because
# the one module declaring it (stems) declared 0.5 — the same number as the
# default — so the declaration and the fallback were indistinguishable until
# stems corrected itself to a measured value and nothing moved.
#
# So this test does not grep for the property name. It extracts each function's
# real source, runs it against three shapes of input, and requires both copies
# to agree on all three.

node - <<'NODE'
const fs = require("fs");

const JS  = "src/shadow/shadow_ui.js";
const MJS = "src/shadow/shadow_ui_tools.mjs";

const fails = [];

/* Pull the function body out of each file and rebuild it against a stub scope,
 * so what runs here is the shipped source rather than a restatement of it. */
function extract(path, usesCtx) {
    const src = fs.readFileSync(path, "utf8");
    const start = src.indexOf("function getToolProcessingRatio()");
    if (start < 0) { fails.push(`${path}: getToolProcessingRatio() not found`); return null; }
    if (src.indexOf("function getToolProcessingRatio()", start + 1) >= 0) {
        fails.push(`${path}: more than one getToolProcessingRatio()`);
        return null;
    }
    /* brace-match to the end of the function */
    let depth = 0, i = src.indexOf("{", start), end = -1;
    for (; i < src.length; i++) {
        if (src[i] === "{") depth++;
        else if (src[i] === "}") { depth--; if (depth === 0) { end = i + 1; break; } }
    }
    if (end < 0) { fails.push(`${path}: unbalanced braces`); return null; }
    const body = src.slice(start, end);
    /* The .mjs copy destructures from ctx; the .js copy closes over globals.
     * Give each the scope it expects. */
    const prelude = usesCtx
        ? "let ctx = { toolSelectedEngine: E, toolActiveTool: T };"
        : "let toolSelectedEngine = E, toolActiveTool = T;";
    return new Function("E", "T", `${prelude}\n${body}\nreturn getToolProcessingRatio();`);
}

const js  = extract(JS, false);
const mjs = extract(MJS, true);

if (js && mjs) {
    const cases = [
        {
            name: "per-engine ratio wins over the module's",
            engine: { processing_ratio: 0.25 },
            tool:   { tool_config: { processing_ratio: 0.85 } },
            expect: 0.25,
        },
        {
            name: "tool_config.processing_ratio is honoured when there is no engine",
            engine: null,
            tool:   { tool_config: { processing_ratio: 0.85 } },
            expect: 0.85,
        },
        {
            name: "falls back to the default when neither declares one",
            engine: null,
            tool:   { tool_config: {} },
            expect: 0.5,
        },
        {
            name: "survives a tool with no tool_config at all",
            engine: null,
            tool:   {},
            expect: 0.5,
        },
        {
            name: "survives no active tool at all",
            engine: null,
            tool:   null,
            expect: 0.5,
        },
    ];

    for (const c of cases) {
        const a = js(c.engine, c.tool);
        const b = mjs(c.engine, c.tool);
        if (a !== c.expect) fails.push(`${JS}: ${c.name} -> ${a}, expected ${c.expect}`);
        if (b !== c.expect) fails.push(`${MJS}: ${c.name} -> ${b}, expected ${c.expect}`);
        if (a !== b) fails.push(`copies disagree on "${c.name}": ${JS}=${a} ${MJS}=${b}`);
    }
}

if (fails.length) {
    for (const f of fails) console.error("FAIL: " + f);
    process.exit(1);
}
console.log("PASS: tool processing ratio (both copies, 5 cases)");
NODE
