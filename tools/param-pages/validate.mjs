#!/usr/bin/env node
/**
 * validate.mjs — report what modules declare versus what the UI can render.
 *
 *   node tools/param-pages/validate.mjs                # whole fleet, summary
 *   node tools/param-pages/validate.mjs --full         # every finding
 *   node tools/param-pages/validate.mjs mrdrums        # one module
 *   node tools/param-pages/validate.mjs --level warn   # filter by severity
 *
 * Reads the checked-in fleet capture. Most findings apply to the existing list
 * editor too — they have simply never been checked for.
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { validateContract, validateFleet } from "../../src/shared/param_pages/validate_contract.mjs";

const ROOT = path.dirname(path.dirname(path.dirname(fileURLToPath(import.meta.url))));
const fx = JSON.parse(fs.readFileSync(path.join(ROOT, "tests", "fixtures", "module-contracts.json"), "utf8"));

const argv = process.argv.slice(2);
const has = (f) => argv.includes("--" + f);
const opt = (f, d) => { const i = argv.indexOf("--" + f); return i >= 0 ? argv[i + 1] : d; };
const only = argv.find((a) => !a.startsWith("--") && argv[argv.indexOf(a) - 1] !== "--level");

const RANK = { error: 0, warn: 1, info: 2 };
const minLevel = RANK[opt("level", "info")] ?? 2;
const tag = (l) => (l === "error" ? "ERROR" : l === "warn" ? "warn " : "info ");

/*
 * Say where the answers come from, on every run.
 *
 * This reads a capture, not a device — so a module migrated after the capture
 * was taken still reports its declared groups as `viz-inferred`, which reads
 * exactly like the migration not having worked. Braids did that for the whole
 * of its first release and cost a session's confusion. The provenance line is
 * cheap; being silently months stale is not.
 */
const STALE_AFTER_DAYS = 30;

function captureProvenance() {
    const when = fx.generated_at ? new Date(fx.generated_at) : null;
    if (!when || Number.isNaN(when.getTime())) {
        return { line: "capture: date unknown — findings reflect a checked-in fixture, not a device", stale: true };
    }
    const days = Math.floor((Date.now() - when.getTime()) / 86400000);
    const age = days <= 0 ? "today" : days === 1 ? "1 day ago" : `${days} days ago`;
    return {
        line: `capture: ${when.toISOString().slice(0, 10)} (${age}), ${fx.module_count ?? fx.modules.length} modules — a fixture, not a live device`,
        stale: days >= STALE_AFTER_DAYS,
    };
}

const prov = captureProvenance();
console.log(prov.line);
if (prov.stale) {
    console.log("  ⚠ modules migrated since then still report as they were captured;");
    console.log("    refresh with tools/param-pages/dump_contracts_device.js before trusting a trend");
}
console.log("");

if (only) {
    const mod = fx.modules.find((m) => m.id === only);
    if (!mod) { console.error(`no module "${only}" in the fixture`); process.exit(2); }
    const { findings } = validateContract({ id: mod.id, hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    console.log(`${mod.id} — ${findings.length} finding(s)`);
    for (const f of findings) {
        if (RANK[f.level] > minLevel) continue;
        console.log(`  [${tag(f.level)}] ${f.rule}\n      ${f.message}`);
    }
    process.exit(0);
}

const { reports, byRule } = validateFleet(fx.modules);

console.log(`Contract validation — ${fx.modules.length} modules, ${reports.length} with findings\n`);
console.log("  sev    rule                      modules");
console.log("  ----------------------------------------------------------------");
for (const r of byRule) {
    if (RANK[r.level] > minLevel) continue;
    console.log(`  ${tag(r.level)}  ${r.rule.padEnd(24)}  ${String(r.modules.length).padStart(2)}  ` +
                r.modules.slice(0, 6).join(", ") + (r.modules.length > 6 ? ", …" : ""));
}

if (has("full")) {
    console.log("\nPer module:\n");
    for (const r of reports) {
        const shown = r.findings.filter((f) => RANK[f.level] <= minLevel);
        if (!shown.length) continue;
        console.log(`== ${r.id}`);
        for (const f of shown) console.log(`  [${tag(f.level)}] ${f.message}`);
        console.log("");
    }
} else {
    console.log("\n  (--full for every finding, or pass a module id)");
}
