#!/usr/bin/env node
/**
 * regenerate.mjs — install a device capture as the test fixture, after showing
 * what it changes.
 *
 *   node tools/param-pages/regenerate.mjs ~/Downloads/module-contracts.json
 *   node tools/param-pages/regenerate.mjs ~/Downloads/module-contracts.json --write
 *
 * Without --write it only reports. A fixture refresh is a reviewed change: in a
 * diff of 563 KB of JSON, a module that regressed its declaration looks exactly
 * like a module that improved it, so the summary is the point.
 *
 * Capture the input with tools/param-pages/dump_contracts_device.js on a Move.
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { diffDumps } from "./dump_contracts.mjs";
import { planPages } from "../../src/shared/param_pages/page_plan.mjs";
import { validateFleet } from "../../src/shared/param_pages/validate_contract.mjs";

const ROOT = path.dirname(path.dirname(path.dirname(fileURLToPath(import.meta.url))));
const FIXTURE = path.join(ROOT, "tests", "fixtures", "module-contracts.json");

const src = process.argv[2];
if (!src) {
    console.error("usage: regenerate.mjs <captured.json> [--write]");
    process.exit(2);
}

const incoming = JSON.parse(fs.readFileSync(src, "utf8"));
const current = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));

if (!Array.isArray(incoming.modules) || incoming.modules.length === 0) {
    console.error("the capture has no modules — refusing to install it");
    process.exit(1);
}
const failed = (incoming.modules || []).filter((m) => m.status !== "ok");
if (failed.length) {
    console.log(`⚠ ${failed.length} module(s) failed to load during capture: ` +
                failed.map((m) => m.id).join(", "));
    console.log("  Installing a capture with load failures loses those modules from the fixture.\n");
}

const d = diffDumps(current, incoming);
console.log(`fixture: ${current.modules.length} modules   capture: ${incoming.modules.length} modules`);
if (d.added.length) console.log("  added:   " + d.added.join(", "));
if (d.removed.length) console.log("  removed: " + d.removed.join(", "));
for (const c of d.changed) console.log(`  changed: ${c.id}  levels ${c.levels}, params ${c.params}`);
if (!d.added.length && !d.removed.length && !d.changed.length) console.log("  no structural change");

/* Page counts are what the tests actually assert on, so show the deltas. */
const pageCount = (m) => planPages({ hierarchy: m.ui_hierarchy, chainParams: m.chain_params }).pages.length;
const before = current.modules.reduce((n, m) => n + pageCount(m), 0);
const after = incoming.modules.reduce((n, m) => n + (m.status === "ok" ? pageCount(m) : 0), 0);
console.log(`\ntotal planned pages: ${before} -> ${after}`);

const v = validateFleet(incoming.modules.filter((m) => m.status === "ok"));
console.log(`contract findings: ${v.reports.length} modules`);
for (const r of v.byRule.filter((r) => r.level !== "info")) {
    console.log(`  ${r.level.padEnd(5)} ${r.rule.padEnd(24)} ${r.modules.length}`);
}

if (!process.argv.includes("--write")) {
    console.log("\n(dry run — pass --write to install, then rerun the param-pages tests" +
                " and refresh snapshots if the renders moved)");
    process.exit(0);
}

/* Keep the fixture trimmed to the declared contract; raw param blobs are what
 * made the original capture 2.2 MB.
 *
 * A module that did not load is recorded by ID and left OUT of `modules`. It
 * published no contract, so an entry with ui_hierarchy: null and
 * chain_params: null is not a contract that renders nothing -- it is the
 * absence of an observation, and every consumer that walks the fixture has to
 * special-case it. The fleet validator does not, and reported the first one to
 * arrive ("work-in: no ui_hierarchy and no chain_params") as a module that
 * cannot render, which is a claim the capture never made. */
const captured = incoming.modules.filter((m) => m.status === "ok");
const out = {
    _source: incoming._source || "Captured from a device.",
    generated_at: incoming.generated_at,
    module_count: captured.length,
    not_captured: failed.map((m) => ({ id: m.id, status: m.status })),
    modules: captured.map((m) => ({
        id: m.id, category: m.category, component_key: m.component_key, status: m.status,
        name: m.name || null, version: m.version || null,
        ui_hierarchy: m.ui_hierarchy || null,
        chain_params: m.chain_params || null,
        presets: m.presets || null,
    })),
};
fs.writeFileSync(FIXTURE, JSON.stringify(out));
console.log(`\nwrote ${FIXTURE} (${Math.round(fs.statSync(FIXTURE).size / 1024)} KB)`);
console.log("next: bash tests/host/test_param_pages_*.sh, then" +
            " UPDATE_SNAPSHOTS=1 bash tests/host/test_param_pages_render.sh if renders moved");
