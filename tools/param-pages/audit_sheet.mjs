#!/usr/bin/env node
/**
 * audit_sheet.mjs — render EVERY page of EVERY module and put them in front of
 * a reviewer one module at a time.
 *
 *   node tools/param-pages/audit_sheet.mjs                    whole fleet -> out/audit/
 *   node tools/param-pages/audit_sheet.mjs obxd minijv        just these
 *   node tools/param-pages/audit_sheet.mjs --cat audio_fx     one category
 *   node tools/param-pages/audit_sheet.mjs --json             report only, no PNGs
 *   node tools/param-pages/audit_sheet.mjs --fixture PATH     audit a fresh dump
 *   node tools/param-pages/audit_sheet.mjs --out DIR --scale 4
 *
 * Then open out/audit/index.html.
 *
 * WHY THIS EXISTS, given preview.mjs already draws a module's pages: because
 * the question here is not "what does obxd look like" but "is anything in the
 * fleet drawn as the wrong TYPE or in a broken LAYOUT", and that question is
 * 759 pages long. A reviewer will not run 95 commands, and the fleet has
 * already taught this codebase that reviewing widgets from their code rather
 * than their render lets real defects through. So: every page, rendered, with
 * the module's findings beside it and a per-module reviewed/flagged mark that
 * survives closing the tab.
 *
 * WHAT THE PICTURES ARE, precisely. Values are synthesised (fake_values.mjs) —
 * this is a LAYOUT audit, not a patch audit. A knob pointing somewhere odd is
 * the dice; a knob where an enum belongs is a defect. The per-cell widget name
 * printed beside each page comes from widgetKindFor(), the same function
 * drawKnobWidget switches on, so "what type is this cell" is answered by the
 * renderer rather than by this file's opinion of the rules.
 *
 * WHAT IT DOES NOT COVER. Overtake modules — they own the whole surface and
 * have no knob grid to plan. Anything the fixture did not capture. And the
 * fixture is a CAPTURE: a module that changed its contract since is drawn as
 * it was, which reads exactly like a bug that has already been fixed. The
 * provenance line prints on every run, and it is the first thing to check
 * before believing a finding.
 *
 * Node-only. Nothing here ships to the device.
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createFramebuffer, drawContext } from "./harness.mjs";
import { fakeValue } from "./fake_values.mjs";
import { planPages, PAGE_KNOBS } from "../../src/shared/param_pages/page_plan.mjs";
import { buildMetaIndex } from "../../src/shared/param_pages/param_meta.mjs";
import { renderPageMovy, widgetKindFor, labelForCell, labelSqueezed }
    from "../../src/shared/param_pages/render_page_movy.mjs";
import { resolveViz } from "../../src/shared/param_pages/viz.mjs";
import { createController, LAYOUT_LIST } from "../../src/shared/param_pages/page_controller.mjs";
import { createFakeDevice } from "./fake_device.mjs";
import { validateContract } from "../../src/shared/param_pages/validate_contract.mjs";

const ROOT = path.dirname(path.dirname(path.dirname(fileURLToPath(import.meta.url))));

const argv = process.argv.slice(2);
const has = (f) => argv.includes("--" + f);
const opt = (f, d = null) => { const i = argv.indexOf("--" + f); return i >= 0 && argv[i + 1] && !argv[i + 1].startsWith("--") ? argv[i + 1] : d; };
const OPTS_WITH_VALUE = new Set(["out", "scale", "fixture", "cat"]);
const ids = argv.filter((a, i) => !a.startsWith("--") && !(i > 0 && argv[i - 1].startsWith("--") && OPTS_WITH_VALUE.has(argv[i - 1].slice(2))));

const FIXTURE = opt("fixture", path.join(ROOT, "tests", "fixtures", "module-contracts.json"));
const OUT = opt("out", path.join(ROOT, "out", "audit"));
const SCALE = parseInt(opt("scale", "4"), 10);
const JSON_ONLY = has("json");

const fx = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));

/* ------------------------------------------------------------------ *
 * Provenance. Same reasoning as validate.mjs: a capture that is months
 * stale reports a module as it WAS, and that is indistinguishable from a
 * live defect unless the run says so out loud, every time.
 * ------------------------------------------------------------------ */
const STALE_AFTER_DAYS = 30;
function provenance() {
    const when = fx.generated_at ? new Date(fx.generated_at) : null;
    if (!when || Number.isNaN(when.getTime())) {
        return { line: "capture: date unknown — a checked-in fixture, not a device", stale: true, iso: null };
    }
    const days = Math.floor((Date.now() - when.getTime()) / 86400000);
    const age = days <= 0 ? "today" : days === 1 ? "1 day ago" : `${days} days ago`;
    return {
        line: `capture: ${when.toISOString().slice(0, 10)} (${age}), ${fx.modules.length} modules — a fixture, not a live device`,
        stale: days >= STALE_AFTER_DAYS,
        iso: when.toISOString(),
    };
}

/* ------------------------------------------------------------------ *
 * Render-time findings.
 *
 * Deliberately ONLY the things a contract check cannot see.
 * validate_contract.mjs already answers every question that can be
 * answered from the declaration — unreachable params, empty ranges,
 * undrawable text, missing hierarchy — and its findings are carried
 * through untouched below. Restating any of them here would give a
 * reviewer two spellings of one defect and a reason to trust neither.
 *
 * What is left needs a framebuffer:
 *
 *   clipped   the page drew OUTSIDE 128x64. The master-FX diagram taught
 *             this one: a row that overflows cannot report that it did,
 *             it just draws off-screen with no error. Counted by the
 *             harness framebuffer, so it catches any drawing at all
 *             leaving the panel, not a shape somebody predicted.
 *   guessed   a cell whose meta was INVENTED — no chain_params entry, so
 *             the grid supplied `float 0..1 step 0.01`. This is the
 *             documented 0.058750-into-an-enum bug, and on a picture it
 *             is invisible: it draws as a perfectly ordinary knob.
 *   squeeze   the label was devowelled or cut to fit its cell. Reported on
 *             the cell, never as a finding — see the note at its call site.
 * ------------------------------------------------------------------ */
const RANK = { error: 0, warn: 1, info: 2 };

function renderModule(mod) {
    const { pages, warnings } = planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const metaIndex = buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });

    /* Contract findings, as-is, from the one validator. */
    const findings = validateContract({ id: mod.id, hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params })
        .findings.map((f) => ({ ...f, page: null }));
    for (const w of warnings) findings.push({ level: "info", rule: "planner", message: w, page: null });

    const rendered = [];
    for (let i = 0; i < pages.length; i++) {
        const p = pages[i];
        const keys = p.keys || [];
        const values = {};
        for (const k of keys) values[k] = fakeValue(k, metaIndex.getOrGuess(k));

        const fb = createFramebuffer();
        const title = `T1 > ${String(mod.name || mod.id).toUpperCase()}`;
        const footer = [["JOG", "PG"], ["SHFT", "SECT"], ["CLK", "MENU"]];

        if (p.kind === PAGE_KNOBS) {
            const { groups } = resolveViz({ keys, metaIndex });
            renderPageMovy(drawContext(fb), {
                page: p, metaIndex, values, title,
                pageIndex: i, pageCount: pages.length, touched: -1, viz: groups, footer,
            });
        } else {
            /*
             * A menu / preset / items page has no grid form, so it is drawn
             * by the REAL controller against a fake device serving this
             * module's own contract — the same path preview.mjs takes. Hand
             * -rendering it here would be previewing a re-implementation of
             * page_controller.mjs, and the point of the sheet is that the
             * picture is the device's.
             */
            const dev = createFakeDevice({ id: mod.id, prefix: "synth", initial: values, serveLists: true });
            const ctrl = createController({
                getParam: dev.getParam, setParam: dev.setParam, announce: () => {}, now: dev.now,
            });
            ctrl.load({ prefix: "synth" });
            ctrl.setLayout(LAYOUT_LIST);
            ctrl.goToPage(i, { remember: false });
            /* One read per tick is the whole point of the cursor — give the
             * page as many ticks as it has values to land, as the device does. */
            for (let t = 0; t < keys.length + 3; t++) ctrl.tick();
            ctrl.render(drawContext(fb), { title, footer });
        }

        /* Per-cell widget, named by the renderer's own classifier. */
        const cells = keys.map((k) => {
            if (!k) return null;
            const meta = metaIndex.getOrGuess(k);
            const label = (meta && meta.name) || k;
            const drawn = labelForCell(label);
            return {
                key: k,
                label,
                drawn,
                widget: p.kind === PAGE_KNOBS ? widgetKindFor(meta) : "row",
                type: (meta && meta.type) || "?",
                guessed: !!(meta && meta.guessed),
                squeezed: p.kind === PAGE_KNOBS && labelSqueezed(label),
                options: meta && Array.isArray(meta.options) ? meta.options.length : null,
            };
        });

        const clipped = fb.clipped();
        if (clipped > 0) {
            findings.push({ level: "error", rule: "clipped", page: i,
                            message: `page ${i} "${p.name}" drew ${clipped} px outside the 128x64 panel` });
        }
        const guessed = cells.filter((c) => c && c.guessed).map((c) => c.key);
        if (guessed.length) {
            findings.push({ level: "warn", rule: "guessed-meta", page: i,
                            message: `page ${i} "${p.name}": invented metadata for ${guessed.join(", ")} — drawn as a plain 0..1 knob because chain_params does not describe it` });
        }
        /*
         * A squeezed label is NOT pushed as a finding.
         *
         * It fires on 597 of the fleet's 759 pages, because a five-character
         * cell is genuinely that tight — as a list it buries the 35 findings
         * that need a decision under 597 that do not. It is real information,
         * so it stays: on the CELL, beside the picture, where the reviewer can
         * see SCATTE and the word it came from in the same glance. That is
         * where a truncation is judged anyway.
         */

        const entry = {
            index: i, kind: p.kind, name: p.name || "", keys: keys.length,
            authored: p.authored !== false, cells, clipped,
        };
        if (!JSON_ONLY) {
            const dir = path.join(OUT, mod.id);
            fs.mkdirSync(dir, { recursive: true });
            const file = path.join(dir, `${String(i).padStart(3, "0")}.png`);
            fs.writeFileSync(file, fb.toPng(SCALE));
            entry.png = `${mod.id}/${path.basename(file)}`;
        }
        rendered.push(entry);
    }

    findings.sort((a, b) => (RANK[a.level] - RANK[b.level]) || ((a.page ?? -1) - (b.page ?? -1)));
    return {
        id: mod.id, name: mod.name || mod.id, category: mod.category,
        version: mod.version || null, pages: rendered, findings,
    };
}

/* ------------------------------------------------------------------ */

const prov = provenance();
console.log(prov.line);
if (prov.stale) {
    console.log("  ⚠ a module migrated since then is drawn as it WAS — refresh with");
    console.log("    tools/param-pages/dump_contracts_device.js before trusting a finding");
}

let picked = fx.modules.filter((m) => m.status !== "load-failed");
if (opt("cat")) picked = picked.filter((m) => m.category === opt("cat"));
if (ids.length) {
    picked = ids.map((id) => {
        const m = fx.modules.find((x) => x.id === id);
        if (!m) { console.error(`no module "${id}" in the fixture`); process.exit(2); }
        return m;
    });
}
picked.sort((a, b) => a.category.localeCompare(b.category) || a.id.localeCompare(b.id));

const report = { generated_from: FIXTURE, capture: prov.iso, modules: [] };
let pageTotal = 0;
for (const m of picked) {
    const r = renderModule(m);
    report.modules.push(r);
    pageTotal += r.pages.length;
    const bad = r.findings.filter((f) => f.level !== "info").length;
    console.log(`  ${r.id.padEnd(20)} ${String(r.pages.length).padStart(3)} pages` +
                (bad ? `   ${bad} finding(s)` : ""));
}

fs.mkdirSync(OUT, { recursive: true });
fs.writeFileSync(path.join(OUT, "report.json"), JSON.stringify(report, null, 2));
if (!JSON_ONLY) {
    fs.writeFileSync(path.join(OUT, "index.html"), buildIndex(report, prov));
}
console.log(`\n${picked.length} modules, ${pageTotal} pages -> ${OUT}`);
if (!JSON_ONLY) console.log(`open ${path.join(OUT, "index.html")}`);

/* ------------------------------------------------------------------ *
 * The reviewer's surface.
 *
 * One module on screen at a time, because that is how the review is
 * actually done — a wall of 759 thumbnails is a picture of the fleet, not
 * a tool for auditing it. [ and ] step modules, and the reviewed / flagged
 * marks persist in localStorage so a fleet pass survives closing the tab.
 * The PNGs are referenced, not inlined: 759 data URIs is a document no
 * browser enjoys and a diff nobody can read.
 * ------------------------------------------------------------------ */
function esc(s) {
    return String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

function buildIndex(rep, prv) {
    const data = JSON.stringify(rep).replace(/</g, "\\u003c");
    return `<!doctype html><meta charset="utf-8"><title>Schwung knob-grid audit</title>
<style>
:root{--bg:#14161a;--fg:#e8e8ea;--dim:#8a8f98;--line:#2a2e35;--warn:#e0a33e;--err:#e05555;--ok:#4caf6d}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace}
header{position:sticky;top:0;background:var(--bg);border-bottom:1px solid var(--line);padding:10px 16px;z-index:2}
h1{font-size:14px;margin:0 0 4px}
.prov{color:var(--dim);font-size:11px}
.stale{color:var(--warn)}
.wrap{display:grid;grid-template-columns:230px 1fr;min-height:calc(100vh - 60px)}
nav{border-right:1px solid var(--line);overflow-y:auto;max-height:calc(100vh - 60px);position:sticky;top:60px}
nav button{display:block;width:100%;text-align:left;background:none;border:0;color:var(--fg);
  font:inherit;padding:4px 10px;cursor:pointer;border-left:3px solid transparent}
nav button:hover{background:#1c1f25}
nav button.sel{background:#22262d;border-left-color:#6aa9ff}
nav .cat{color:var(--dim);font-size:10px;padding:10px 10px 2px;text-transform:uppercase;letter-spacing:.08em}
nav .done{color:var(--ok)} nav .flag{color:var(--err)}
main{padding:16px 20px;overflow-x:hidden}
.modhead{display:flex;align-items:baseline;gap:12px;flex-wrap:wrap;margin-bottom:10px}
.modhead h2{font-size:18px;margin:0}
.modhead .meta{color:var(--dim)}
.marks button{background:#1c1f25;border:1px solid var(--line);color:var(--fg);font:inherit;
  padding:3px 10px;border-radius:4px;cursor:pointer;margin-left:6px}
.marks button.on{background:#2b6b45;border-color:#357f52}
.marks button.on.flagbtn{background:#6b2b2b;border-color:#7f3535}
.findings{margin:0 0 14px;padding:8px 12px;border:1px solid var(--line);border-radius:5px;background:#181b20}
.findings div{margin:2px 0}
.lv{display:inline-block;min-width:52px;font-size:10px;letter-spacing:.06em}
.error .lv{color:var(--err)} .warn .lv{color:var(--warn)} .info .lv{color:var(--dim)}
.rule{color:#6aa9ff}
.pages{display:flex;flex-wrap:wrap;gap:16px}
figure{margin:0;width:512px;max-width:100%}
figure img{width:100%;image-rendering:pixelated;display:block;background:#000;border:1px solid var(--line)}
figure.bad img{border-color:var(--err)}
figcaption{color:var(--dim);font-size:11px;padding:4px 2px 0}
.cells{color:var(--dim);font-size:11px;display:flex;flex-wrap:wrap;gap:2px 10px}
.cells span b{color:var(--fg);font-weight:400}
.cells span.g{color:var(--warn)}
.cells span.sq i{color:#5d6570;font-style:normal}
.cells span.sq i::before{content:"\\2190"}
.w-enum{color:#c58fe0}.w-knob{color:#6aa9ff}.w-bignum{color:#4caf6d}.w-opaque{color:#e0a33e}.w-button{color:#e07f7f}
kbd{background:#22262d;border:1px solid var(--line);border-radius:3px;padding:0 4px}
</style>
<header>
<h1>Schwung knob-grid audit — ${rep.modules.length} modules</h1>
<div class="prov${prv.stale ? " stale" : ""}">${esc(prv.line)}${prv.stale ? " · refresh with dump_contracts_device.js before trusting a finding" : ""}
 · values are synthesised: judge layout and widget type, not the reading
 · <kbd>[</kbd> <kbd>]</kbd> module &nbsp; <kbd>r</kbd> reviewed &nbsp; <kbd>f</kbd> flag &nbsp; <kbd>i</kbd> info findings</div>
</header>
<div class="wrap"><nav id="nav"></nav><main id="main"></main></div>
<script>
const REPORT = ${data};
const KEY = "schwung-audit-marks";
let marks = {};
try { marks = JSON.parse(localStorage.getItem(KEY) || "{}"); } catch (e) { marks = {}; }
const saveMarks = () => localStorage.setItem(KEY, JSON.stringify(marks));
let sel = 0;
/* info findings are 90% of the list (viz-inferred alone is 599) and none of
 * them ask for a decision, so they are off until asked for. */
let showInfo = false;

function drawNav() {
  const nav = document.getElementById("nav");
  nav.innerHTML = "";
  let cat = null;
  REPORT.modules.forEach((m, i) => {
    if (m.category !== cat) {
      cat = m.category;
      const h = document.createElement("div");
      h.className = "cat"; h.textContent = cat;
      nav.appendChild(h);
    }
    const b = document.createElement("button");
    const mk = marks[m.id] || {};
    b.className = (i === sel ? "sel " : "") + (mk.flag ? "flag" : mk.done ? "done" : "");
    b.textContent = (mk.flag ? "\\u2691 " : mk.done ? "\\u2713 " : "  ") + m.id + "  (" + m.pages.length + ")";
    b.onclick = () => { sel = i; render(); };
    nav.appendChild(b);
  });
  const cur = nav.querySelector("button.sel");
  if (cur) cur.scrollIntoView({ block: "nearest" });
}

function render() {
  const m = REPORT.modules[sel];
  const mk = marks[m.id] || {};
  const main = document.getElementById("main");
  const f = m.findings.filter(x => showInfo || x.level !== "info").map(x =>
    '<div class="' + x.level + '"><span class="lv">' + x.level.toUpperCase() + '</span> ' +
    '<span class="rule">' + x.rule + '</span> ' + esc(x.message) + '</div>').join("");
  const pages = m.pages.map(p => {
    const cells = (p.cells || []).filter(Boolean).map(c =>
      '<span class="' + (c.guessed ? "g " : "") + (c.squeezed ? "sq" : "") +
      '" title="' + esc(c.key) + ' \u2014 ' + esc(c.type) +
      (c.options ? ', ' + c.options + ' options' : '') + '">' +
      '<b class="w-' + c.widget + '">' + c.widget + '</b> ' + esc(c.drawn) +
      (c.squeezed ? '<i>' + esc(c.label) + '</i>' : '') +
      (c.guessed ? " (guessed)" : "") + '</span>').join("");
    return '<figure' + (p.clipped ? ' class="bad"' : '') + '>' +
      (p.png ? '<img loading="lazy" src="' + p.png + '" alt="page ' + p.index + '">' : '') +
      '<figcaption>' + p.index + ' &middot; ' + p.kind + ' &middot; "' + esc(p.name) + '" &middot; ' +
      p.keys + ' keys' + (p.authored ? '' : ' &middot; OVERFLOW (planner-paginated)') +
      (p.clipped ? ' &middot; CLIPPED ' + p.clipped + 'px' : '') +
      '<div class="cells">' + cells + '</div></figcaption></figure>';
  }).join("");
  main.innerHTML =
    '<div class="modhead"><h2>' + esc(m.name) + '</h2>' +
    '<span class="meta">' + m.id + ' &middot; ' + m.category +
    (m.version ? ' &middot; v' + esc(m.version) : '') + ' &middot; ' + m.pages.length + ' pages</span>' +
    '<span class="marks"><button class="' + (mk.done ? "on" : "") + '" onclick="mark(\\'done\\')">reviewed</button>' +
    '<button class="flagbtn ' + (mk.flag ? "on" : "") + '" onclick="mark(\\'flag\\')">flag</button>' +
    '<button class="' + (showInfo ? "on" : "") + '" onclick="toggleInfo()">info</button></span></div>' +
    (f ? '<div class="findings">' + f + '</div>'
       : '<div class="findings"><span class="lv">\u2014</span> no findings above info level</div>') +
    '<div class="pages">' + pages + '</div>';
  main.scrollTop = 0;
  window.scrollTo(0, 0);
  drawNav();
  /*
   * replaceState, not an assignment to location.hash.
   *
   * (No backticks or dollar-braces in this half of the file: everything below
   * buildIndex lives inside ONE template literal, so either would end the
   * string mid-page and the error surfaces hundreds of lines away.)
   *
   * Assigning the hash pushes a history entry per module, so Back walks
   * backwards through the fleet one module at a time instead of leaving the
   * sheet — 95 presses to get out. The hash is here to make a finding
   * SHAREABLE ("look at #sfz"), not to be a navigation stack.
   */
  history.replaceState(null, "", "#" + m.id);
}

/* A hash typed or pasted into an ALREADY-OPEN tab is a same-document change:
 * no reload, so the load-time hash read below never runs again and the link
 * silently does nothing. Which is the case that matters, because the link is
 * shared to someone who already has the sheet open. */
addEventListener("hashchange", () => {
  const id = location.hash.slice(1);
  const i = REPORT.modules.findIndex(m => m.id === id);
  if (i >= 0 && i !== sel) { sel = i; render(); }
});

function mark(what) {
  const id = REPORT.modules[sel].id;
  marks[id] = marks[id] || {};
  marks[id][what] = !marks[id][what];
  saveMarks(); render();
}
function toggleInfo(){ showInfo = !showInfo; render(); }
function esc(s){ return String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }

addEventListener("keydown", e => {
  if (e.target.tagName === "INPUT") return;
  if (e.key === "]") { sel = Math.min(REPORT.modules.length - 1, sel + 1); render(); }
  else if (e.key === "[") { sel = Math.max(0, sel - 1); render(); }
  else if (e.key === "r") mark("done");
  else if (e.key === "f") mark("flag");
  else if (e.key === "i") toggleInfo();
});

const want = location.hash.slice(1);
if (want) { const i = REPORT.modules.findIndex(m => m.id === want); if (i >= 0) sel = i; }
render();
</script>`;
}
