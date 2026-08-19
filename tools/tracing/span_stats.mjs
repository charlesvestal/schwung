/**
 * span_stats.mjs — turn OTLP/JSONL trace dumps into a duration report.
 *
 * `docs/tracing.md` explains how to arm tracing and pull the files; this is
 * what reads them without standing up Tempo or Jaeger. A collector is the
 * right tool for exploring one trace; this is the right tool for the question
 * "what does this operation cost, over thousands of samples" — which is the
 * question a perf claim in a code comment has to answer.
 *
 *   node tools/tracing/span_stats.mjs traces/*.otlp.jsonl
 *   node tools/tracing/span_stats.mjs --name param.set traces/*.jsonl
 *   node tools/tracing/span_stats.mjs --children param.set traces/*.jsonl
 *
 * Reports count, mean, median, p95, p99 and max per span name. Percentiles
 * matter more than the mean here: an operation that waits for the next SPI
 * frame is quantised, not normally distributed, so the mean hides both the
 * cheap path and the worst case a user actually feels.
 *
 * --children <name> additionally breaks <name> down by what nested inside it,
 * including the share of time NOT covered by any child ("unaccounted"), which
 * is where a busy-wait with no span of its own shows up.
 */

import fs from "node:fs";

const args = process.argv.slice(2);
const files = [];
let filterName = null, childrenOf = null, histogramOf = null;
for (let i = 0; i < args.length; i++) {
    if (args[i] === "--name") filterName = args[++i];
    else if (args[i] === "--children") childrenOf = args[++i];
    else if (args[i] === "--histogram") histogramOf = args[++i];
    else if (args[i] === "--help" || args[i] === "-h") { usage(); process.exit(0); }
    else files.push(args[i]);
}
if (!files.length) { usage(); process.exit(1); }

function usage() {
    console.log(`usage: node tools/tracing/span_stats.mjs [--name N] [--children N] [--histogram N] <file.otlp.jsonl ...>

  --name N        report only spans called N
  --children N    break N down by nested span, incl. unaccounted time
  --histogram N   bucket N's durations, to see quantisation`);
}

/* ---- parse ------------------------------------------------------------- */

const spans = [];
for (const f of files) {
    let text;
    try { text = fs.readFileSync(f, "utf8"); }
    catch (e) { console.error(`skipping ${f}: ${e.message}`); continue; }
    let lineNo = 0, bad = 0;
    for (const line of text.split("\n")) {
        lineNo++;
        if (!line.trim()) continue;
        let obj;
        /* The exporter flushes per batch; a file pulled while tracing is still
         * running can end mid-line. Skip the torn tail rather than refuse the
         * whole capture. */
        try { obj = JSON.parse(line); } catch (e) { bad++; continue; }
        for (const rs of obj.resourceSpans || []) {
            const svc = (rs.resource?.attributes || [])
                .find((a) => a.key === "service.name")?.value?.stringValue || "?";
            for (const ss of rs.scopeSpans || []) {
                for (const s of ss.spans || []) {
                    const t0 = BigInt(s.startTimeUnixNano), t1 = BigInt(s.endTimeUnixNano);
                    spans.push({
                        svc,
                        name: s.name,
                        id: s.spanId,
                        parent: s.parentSpanId || null,
                        trace: s.traceId,
                        t0, t1,
                        ns: Number(t1 - t0),
                    });
                }
            }
        }
    }
    if (bad) console.error(`${f}: skipped ${bad} unparseable line(s) (torn tail?)`);
}

if (!spans.length) { console.error("no spans found"); process.exit(1); }

/* ---- stats ------------------------------------------------------------- */

function pct(sorted, p) {
    if (!sorted.length) return 0;
    const i = Math.min(sorted.length - 1, Math.max(0, Math.ceil((p / 100) * sorted.length) - 1));
    return sorted[i];
}
function fmt(ns) {
    if (ns >= 1e6) return (ns / 1e6).toFixed(2) + "ms";
    if (ns >= 1e3) return (ns / 1e3).toFixed(1) + "us";
    return ns.toFixed(0) + "ns";
}
function pad(s, n) { s = String(s); return s.length >= n ? s : s + " ".repeat(n - s.length); }
function lpad(s, n) { s = String(s); return s.length >= n ? s : " ".repeat(n - s.length) + s; }

const byName = new Map();
for (const s of spans) {
    if (filterName && s.name !== filterName) continue;
    const k = s.svc + "  " + s.name;
    if (!byName.has(k)) byName.set(k, []);
    byName.get(k).push(s.ns);
}

console.log(`\n${spans.length} spans from ${files.length} file(s)\n`);
console.log(pad("service / span", 40) + lpad("count", 8) + lpad("mean", 10) +
            lpad("p50", 10) + lpad("p95", 10) + lpad("p99", 10) + lpad("max", 10) + lpad("total", 11));
console.log("-".repeat(109));

const rows = [...byName.entries()].map(([k, arr]) => {
    const sorted = arr.slice().sort((a, b) => a - b);
    const total = arr.reduce((a, b) => a + b, 0);
    return { k, n: arr.length, mean: total / arr.length, p50: pct(sorted, 50),
             p95: pct(sorted, 95), p99: pct(sorted, 99), max: sorted[sorted.length - 1], total };
});
rows.sort((a, b) => b.total - a.total);
for (const r of rows) {
    console.log(pad(r.k, 40) + lpad(r.n, 8) + lpad(fmt(r.mean), 10) + lpad(fmt(r.p50), 10) +
                lpad(fmt(r.p95), 10) + lpad(fmt(r.p99), 10) + lpad(fmt(r.max), 10) +
                lpad(fmt(r.total), 11));
}

/* ---- child breakdown --------------------------------------------------- */

if (childrenOf) {
    const byId = new Map(spans.map((s) => [s.id, s]));
    const kids = new Map();          /* parentId -> [child] */
    for (const s of spans) {
        if (!s.parent) continue;
        if (!kids.has(s.parent)) kids.set(s.parent, []);
        kids.get(s.parent).push(s);
    }
    const parents = spans.filter((s) => s.name === childrenOf);
    if (!parents.length) {
        console.log(`\nno spans named ${childrenOf}`);
    } else {
        const agg = new Map();
        let unaccounted = 0, parentTotal = 0;
        for (const p of parents) {
            parentTotal += p.ns;
            let covered = 0;
            for (const c of (kids.get(p.id) || [])) {
                covered += c.ns;
                agg.set(c.name, (agg.get(c.name) || 0) + c.ns);
            }
            /* Clamp: a child in another process can overhang its parent by a
             * hair on a different clock read. Negative would be noise, not a
             * finding. */
            unaccounted += Math.max(0, p.ns - covered);
        }
        console.log(`\n${childrenOf}: ${parents.length} spans, ${fmt(parentTotal)} total — where it goes:`);
        const items = [...agg.entries()].sort((a, b) => b[1] - a[1]);
        items.push(["(unaccounted — no child span)", unaccounted]);
        for (const [name, ns] of items) {
            const share = parentTotal ? (100 * ns / parentTotal).toFixed(1) : "0.0";
            console.log("  " + pad(name, 38) + lpad(fmt(ns), 11) + lpad(share + "%", 9));
        }
        /* Cross-process children are the point of the trace-id propagation, so
         * say plainly whether any showed up. */
        const svcs = new Set();
        for (const p of parents) for (const c of (kids.get(p.id) || [])) svcs.add(c.svc);
        if (svcs.size) console.log("  child services seen: " + [...svcs].join(", "));
        else console.log("  no child spans found — is the other service's file included?");
    }
}

/* ---- histogram --------------------------------------------------------- */

if (histogramOf) {
    const arr = spans.filter((s) => s.name === histogramOf).map((s) => s.ns).sort((a, b) => a - b);
    if (!arr.length) {
        console.log(`\nno spans named ${histogramOf}`);
    } else {
        console.log(`\n${histogramOf}: ${arr.length} samples, distribution`);
        /* Fixed sub-millisecond buckets: the hypothesis under test is that this
         * path is quantised by the ~2.9ms SPI frame, and even buckets across
         * the range would smear exactly the structure we are looking for. */
        const edges = [0, 100e3, 250e3, 500e3, 1e6, 1.5e6, 2e6, 2.9e6, 4e6, 6e6, 10e6, Infinity];
        const counts = new Array(edges.length - 1).fill(0);
        for (const v of arr) {
            for (let i = 0; i < counts.length; i++) {
                if (v < edges[i + 1]) { counts[i]++; break; }
            }
        }
        const maxCount = Math.max(...counts);
        for (let i = 0; i < counts.length; i++) {
            if (!counts[i]) continue;
            const lo = fmt(edges[i]), hi = edges[i + 1] === Infinity ? "+" : fmt(edges[i + 1]);
            const bar = "#".repeat(Math.max(1, Math.round(40 * counts[i] / maxCount)));
            console.log("  " + lpad(lo, 8) + " .. " + pad(hi, 8) + lpad(counts[i], 7) + "  " + bar);
        }
    }
}
console.log("");
