/**
 * validate_contract.mjs — check what a module declares against what the UI can
 * actually render.
 *
 * PURE, like the rest of the library: give it a parsed `ui_hierarchy` and
 * `chain_params` and it returns findings. Useful well beyond the knob grid —
 * most of these problems affect the existing list editor too, they have simply
 * never been checked for.
 *
 * Every rule here exists because a real module in the 76-module fleet trips it.
 * Nothing is hypothetical; see docs/plans/2026-07-26-param-pages-audit.md.
 *
 * Severities:
 *   error  the module will not render usefully
 *   warn   something the user will notice and the author probably did not mean
 *   info   worth knowing, not necessarily wrong
 */

import { buildMetaIndex, KIND_OPAQUE } from "./param_meta.mjs";
import { planPages, PAGE_KNOBS, KNOBS_PER_PAGE } from "./page_plan.mjs";
import { hasChildren, allChildKeys } from "./child_key.mjs";
import { resolveViz, VIZ_SOURCE_DECLARED } from "./viz.mjs";

/* Types the contract documents, plus the ones modules actually ship.
 * `toggle` is used inline by real modules but is absent from docs/MODULES.md —
 * that is a documentation bug, not a module bug, so it is accepted here and
 * reported once as info. */
const DOCUMENTED_TYPES = new Set([
    "float", "int", "enum", "string", "filepath", "file",
    "canvas", "wav_position", "note", "rate",
    "module_picker", "parameter_picker",
]);
const UNDOCUMENTED_BUT_USED = new Set(["toggle"]);

const NON_ASCII = /[^\x20-\x7e]/;

function keyOf(entry) {
    if (typeof entry === "string") return entry;
    if (entry && typeof entry === "object" && entry.key) return entry.key;
    return null;
}

/**
 * @param {object} o
 * @param {string} o.id            module id, for the report
 * @param {object} o.hierarchy     parsed ui_hierarchy (may be null)
 * @param {Array}  o.chainParams   parsed chain_params (may be null)
 * @param {object} [o.capabilities] parsed module.json capabilities. OPTIONAL,
 *        and checks that need it stay silent when it is absent — no existing
 *        caller passes it, and a contract validated without it must not grow a
 *        warning it has no way to answer.
 * @returns {{id:string, findings:Array<{level:string, rule:string, message:string}>}}
 */
export function validateContract({ id, hierarchy, chainParams, capabilities } = {}) {
    const findings = [];
    const add = (level, rule, message) => findings.push({ level, rule, message });

    const cp = chainParams || [];
    const levels = (hierarchy && hierarchy.levels) || null;

    /* ---- structural ---------------------------------------------------- */

    if (!levels && cp.length === 0) {
        add("error", "nothing-declared",
            "no ui_hierarchy and no chain_params — nothing can be rendered");
        return { id, findings };
    }
    if (!levels) {
        add("warn", "no-hierarchy",
            `no ui_hierarchy; ${cp.length} chain_params will be paginated in declaration order, ` +
            "with no grouping or names beyond the keys");
    }

    /* ---- chain_params hygiene ------------------------------------------ */

    /* Params whose range is populated at runtime rather than declared: a preset
     * or item index is sized by its count_param, so hush1/sf2 ship "preset"
     * as 0..-1 and osirus ships "bank_index" as 0..0. That is the contract
     * working as intended — the preset page reads the live count — so the
     * empty-range rule must not fire on them. A validator that cries wolf
     * stops being read. */
    const runtimeRanged = new Set();
    for (const lvl of Object.values(levels || {})) {
        if (!lvl || typeof lvl !== "object") continue;
        for (const f of ["list_param", "select_param", "count_param"]) {
            if (lvl[f]) runtimeRanged.add(lvl[f]);
        }
    }

    /*
     * A CUSTOM KIND WITH NO SCRIPT IS THE ONE MISTAKE WORTH REPORTING.
     *
     * An unknown custom kind is LEGAL. It is exactly what an older host sees
     * when it reads a newer module, and it degrades on its own: an unregistered
     * kind claims nothing, so the detector picks the keys up and the built-in
     * draws. Flagging that would make every forward-compatible module look
     * broken.
     *
     * But a module that declares custom: and ships no canvas_script has simply
     * forgotten the file, and NOTHING ELSE in the system will ever say so --
     * the widget silently never appears, and what the user sees instead is a
     * perfectly reasonable built-in picture.
     *
     * Silent when `capabilities` was not supplied: the caller cannot answer the
     * question, so asking it would only produce noise.
     */
    if (capabilities) {
        const customKinds = new Set();
        for (const p of cp) {
            const k = p && p.viz && p.viz.kind;
            if (typeof k === "string" && k.startsWith("custom:")) customKinds.add(k);
        }
        if (customKinds.size > 0 && !capabilities.canvas_script) {
            add("warn", "custom-widget-no-script",
                `declares custom viz kind(s) ${[...customKinds].join(", ")} but the module ` +
                "ships no canvas_script — the widget will never load and the built-in " +
                "will draw in its place");
        }
    }

    const seen = new Set();
    for (const p of cp) {
        if (!p || !p.key) { add("warn", "param-without-key", "a chain_params entry has no key"); continue; }
        if (seen.has(p.key)) add("warn", "duplicate-param", `chain_params declares "${p.key}" more than once`);
        seen.add(p.key);

        const type = String(p.type || "").toLowerCase();
        if (type && !DOCUMENTED_TYPES.has(type) && !UNDOCUMENTED_BUT_USED.has(type)) {
            add("warn", "unknown-type", `"${p.key}" declares type "${p.type}", which the UI does not know`);
        }
        if (UNDOCUMENTED_BUT_USED.has(type)) {
            add("info", "undocumented-type",
                `"${p.key}" uses type "${type}", which works but is missing from docs/MODULES.md`);
        }
        if (p.ui_type && type && String(p.ui_type).toLowerCase() !== type &&
            !(type === "float" || type === "int")) {
            add("warn", "type-ui_type-conflict",
                `"${p.key}" declares type "${p.type}" and ui_type "${p.ui_type}"`);
        }
        if (type === "enum" && (!Array.isArray(p.options) || p.options.length === 0)) {
            add("warn", "enum-without-options",
                `"${p.key}" is an enum with no options; the UI can only show a bare index`);
        }
        if (typeof p.min === "number" && typeof p.max === "number" && p.max <= p.min) {
            if (runtimeRanged.has(p.key)) {
                add("info", "runtime-ranged",
                    `"${p.key}" declares an empty range (${p.min}..${p.max}) because its size comes ` +
                    "from count_param at runtime — expected for a preset or item index");
            } else {
                add("warn", "empty-range",
                    `"${p.key}" has max (${p.max}) <= min (${p.min}), so a knob cannot move it. ` +
                    "If the real range is only known at runtime, declare it through a level's " +
                    "count_param so the UI can size the control.");
            }
        }
    }

    /* ---- text the display cannot draw ---------------------------------- */

    const badText = [];
    const checkText = (where, s) => {
        if (typeof s === "string" && NON_ASCII.test(s)) {
            badText.push(`${where}: ${JSON.stringify(s)}`);
        }
    };
    for (const p of cp) {
        if (!p) continue;
        checkText(`${p.key}.name`, p.name);
        checkText(`${p.key}.unit`, p.unit);
        for (const o of (p.options || [])) checkText(`${p.key}.options`, o);
    }
    for (const [lname, lvl] of Object.entries(levels || {})) {
        if (!lvl || typeof lvl !== "object") continue;
        checkText(`level ${lname}.name`, lvl.name);
        checkText(`level ${lname}.label`, lvl.label);
        for (const p of (lvl.params || [])) {
            if (!p || typeof p !== "object") continue;
            checkText(`${lname}.${p.key || p.level}.label`, p.label);
            for (const o of (p.options || [])) checkText(`${lname}.${p.key}.options`, o);
        }
    }
    if (badText.length) {
        add("warn", "undrawable-text",
            `${badText.length} string(s) contain characters the 5x7 device font cannot draw ` +
            `(they render as nothing): ${badText.slice(0, 4).join("; ")}` +
            (badText.length > 4 ? ` and ${badText.length - 4} more` : ""));
    }

    if (!levels) return { id, findings };

    /* ---- level hygiene -------------------------------------------------- */

    const declaredKeys = new Set();
    for (const [lname, lvl] of Object.entries(levels)) {
        if (!lvl || typeof lvl !== "object") {
            add("warn", "bad-level", `level "${lname}" is not an object`);
            continue;
        }
        const knobs = (lvl.knobs || []).map(keyOf).filter(Boolean);
        for (const k of knobs) declaredKeys.add(k);
        for (const p of (lvl.params || [])) {
            const k = keyOf(p);
            if (k && !(p && p.level)) declaredKeys.add(k);
        }
        /* A preset index or item selector is reached through its own page kind,
         * not through knobs[]/params[] — it is declared, just not listed. */
        for (const f of ["list_param", "count_param", "name_param", "items_param", "select_param", "mode_param"]) {
            if (lvl[f]) declaredKeys.add(lvl[f]);
        }

        if (lvl.children === "None") {
            add("info", "children-none-string",
                `level "${lname}" spells an absent child as the string "None" rather than null`);
        }
        if (knobs.length > KNOBS_PER_PAGE) {
            add("info", "level-over-eight",
                `level "${lname}" declares ${knobs.length} knobs; it will span ` +
                `${Math.ceil(knobs.length / KNOBS_PER_PAGE)} pages`);
        }
        if (lvl.list_param && !lvl.count_param) {
            add("warn", "preset-without-count",
                `level "${lname}" has list_param but no count_param; the preset browser cannot size itself`);
        }
        if (lvl.items_param && !lvl.select_param) {
            add("warn", "items-without-select",
                `level "${lname}" has items_param but no select_param; the list cannot commit a choice`);
        }
        if ((lvl.child_prefix || lvl.child_key_template) && !(lvl.child_count > 0)) {
            add("warn", "child-without-count",
                `level "${lname}" declares repeated elements but no positive child_count`);
        }
        if (lvl.child_key_overrides && !lvl.child_key_template && !lvl.child_prefix) {
            add("warn", "child-overrides-without-template",
                `level "${lname}" has child_key_overrides but no template to override`);
        }
    }

    /* ---- reachability ---------------------------------------------------- */

    const { pages, warnings } = planPages({ hierarchy, chainParams: cp });
    for (const w of warnings) add("info", "planner", w);

    const onPage = new Set();
    for (const p of pages) for (const k of (p.keys || [])) onPage.add(k);

    const unreachable = [...seen].filter((k) => !declaredKeys.has(k));
    if (unreachable.length) {
        /* Keys a child level addresses per instance are declared, just not
         * listed one by one — that is the entire point of the mechanism. */
        const viaChildren = new Set();
        for (const lvl of Object.values(levels)) {
            if (hasChildren(lvl)) for (const k of allChildKeys(lvl)) viaChildren.add(k);
        }
        /*
         * A READ-ONLY param is not meant to be reached.
         *
         * `access: "read"` is telemetry -- a meter, a detected key, a sample
         * count. It is published so a web panel or a remote client can display
         * it, and putting it on a knob page would be a defect, not a fix. But
         * this rule counted it as unreachable and told the author to go and
         * place it: 4k-eq declares in_peak_l/r, out_peak_l/r and clip exactly
         * as designed, and was reported as having 7 unreachable params of
         * which 5 were correct.
         *
         * That mattered beyond the noise. The count is what a reviewer reads
         * first, and a rule that inflates it with things nobody should act on
         * teaches them to stop reading it -- so the two REAL gaps in that same
         * module (hpf_enabled and lpf_enabled, filter switches with no way to
         * reach them from the device) sat behind five false ones.
         *
         * Write-only is deliberately NOT excluded: a trigger is a button, and a
         * button you cannot press is a real gap.
         */
        const readOnly = new Set(
            cp.filter((p) => p && p.key && String(p.access || "").toLowerCase() === "read")
              .map((p) => p.key));
        const real = unreachable.filter((k) => !viaChildren.has(k) && !readOnly.has(k));
        if (real.length) {
            add("warn", "unreachable-params",
                `${real.length} chain_params are listed in no level, so no UI can reach them: ` +
                `${real.slice(0, 6).join(", ")}${real.length > 6 ? ", …" : ""}`);
        }
    }

    /* ---- metadata gaps ---------------------------------------------------- */

    const index = buildMetaIndex({ hierarchy, chainParams: cp });
    const guessed = [];
    let opaqueOnKnobs = 0;
    for (const page of pages) {
        if (page.kind !== PAGE_KNOBS) continue;
        for (const k of page.keys) {
            const meta = index.getOrGuess(k);
            if (meta.guessed) guessed.push(k);
            if (meta.kind === KIND_OPAQUE) opaqueOnKnobs++;
        }
    }
    if (guessed.length) {
        add("warn", "undeclared-knob-params",
            `${guessed.length} knob param(s) have no chain_params entry and no inline metadata, ` +
            `so type and range are guessed: ${guessed.slice(0, 6).join(", ")}`);
    }
    if (index.conflicts.length) {
        add("warn", "inline-metadata-conflict",
            `levels disagree about the metadata for: ${index.conflicts.join(", ")}`);
    }
    if (opaqueOnKnobs) {
        add("info", "opaque-on-knobs",
            `${opaqueOnKnobs} knob slot(s) hold a param a knob cannot turn ` +
            "(filepath / wav_position / canvas / string); they open an editor on click");
    }

    /* ---- graphics: declared vs guessed ------------------------------------
     *
     * "Silent wrongness was the real objection to detectors; visible
     * wrongness is fine" — docs/plans/2026-07-26-param-pages-audit.md §13.5.
     * So every group the detector fires is reported, not just the ones that
     * look suspicious: an author who never looks cannot correct a guess they
     * do not know was made. */
    const declared = [], inferred = [], invalidDeclared = [];
    for (const page of pages) {
        if (page.kind !== PAGE_KNOBS) continue;
        const { groups, invalid } = resolveViz({ keys: page.keys, metaIndex: index });
        for (const g of groups) {
            (g.source === VIZ_SOURCE_DECLARED ? declared : inferred).push(g);
        }
        for (const v of invalid) invalidDeclared.push(v);
    }
    if (inferred.length) {
        for (const g of inferred) {
            add("info", "viz-inferred",
                `a ${g.kind} graphic was inferred over ${g.keys.join(", ")} — declare a "viz" ` +
                "field on these params in chain_params to confirm it or correct it");
        }
    }
    if (invalidDeclared.length) {
        for (const v of invalidDeclared) {
            add("warn", "viz-declared-not-adjacent",
                `viz group "${v.group}" (${v.kind}) is declared but its roles are not adjacent ` +
                "on one page row, so no graphic is drawn — move the params next to each other");
        }
    }
    if (declared.length) {
        add("info", "viz-declared",
            `${declared.length} graphic(s) declared via chain_params "viz"`);
    }

    return { id, findings };
}

/** Convenience: validate a whole fleet capture and summarise by rule. */
export function validateFleet(modules) {
    const reports = [];
    const byRule = new Map();
    for (const m of modules) {
        const r = validateContract({ id: m.id, hierarchy: m.ui_hierarchy, chainParams: m.chain_params });
        if (r.findings.length) reports.push(r);
        for (const f of r.findings) {
            if (!byRule.has(f.rule)) byRule.set(f.rule, { rule: f.rule, level: f.level, modules: [] });
            const e = byRule.get(f.rule);
            if (!e.modules.includes(m.id)) e.modules.push(m.id);
        }
    }
    return { reports, byRule: [...byRule.values()].sort((a, b) => b.modules.length - a.modules.length) };
}
