#!/usr/bin/env node
// Pairwise A/B comparator for the SCH-50 style catalog.
//
// Picking one from a ten-up contact sheet is a bad ask: it is a ten-way choice
// and it yields a single pick with no information about the other nine. This
// serves pairs instead and appends every judgement to disk as it lands, so a
// session that is killed halfway loses nothing.
//
// Option names, notes and axis positions are hidden until AFTER the choice.
// The minimal->radical axis each set is ordered on is an authored HYPOTHESIS;
// the preference data is what tests it, so showing it beforehand would bias the
// data meant to test it.
//
//   node tools/param-pages/ab_server.mjs [--port 7788]
//
// Dependency-free: node:http, node:fs, node:path, node:url only.

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..');
const OUT_DIR = path.join(REPO, 'catalog-out');
const PREFS = path.join(OUT_DIR, 'preferences.json');
const STYLES = path.join(REPO, 'src', 'shared', 'param_pages', 'styles', 'index.mjs');

const HOST = '127.0.0.1';
const TARGET_PER_SET = 16;

/*
 * 7789, not 7788, and the difference is a silent wrong-page failure.
 *
 * A long-running `node tools/webui_serve.mjs --port 7788` binds the IPv6
 * wildcard. Binding 127.0.0.1:7788 alongside it SUCCEEDS -- the more specific
 * IPv4 address does not collide with `*` on v6 -- so nothing appears wrong from
 * here. But a browser given `localhost:7788` resolves ::1 first on macOS and
 * lands on the OTHER server, showing a working page that is not this one.
 *
 * A port this tool owns avoids the whole class. `--port` remains for when 7789
 * is busy too.
 */
let PORT = 7789;
for (let i = 2; i < process.argv.length; i++) {
    if (process.argv[i] === '--port') PORT = parseInt(process.argv[i + 1], 10) || PORT;
}

// ---------------------------------------------------------------- catalog

const { SETS } = await import(pathToFileURL(STYLES).href);

function pad2(n) { return String(n).padStart(2, '0'); }

// An option is only offerable if BOTH its renders exist on disk. A set is only
// offerable if it is registered AND at least two of its options rendered --
// one option cannot be compared with anything.
function scanCatalog() {
    const ready = [];
    const missing = [];
    for (const set of SETS) {
        const dir = path.join(OUT_DIR, set.id);
        if (!fs.existsSync(dir)) { missing.push({ set: set.id, why: 'no catalog-out/' + set.id }); continue; }
        const opts = [];
        const absent = [];
        for (const o of set.options) {
            const page = `${pad2(o.position)}-${o.id}-page.png`;
            const swatch = `${pad2(o.position)}-${o.id}-swatch.png`;
            if (fs.existsSync(path.join(dir, page)) && fs.existsSync(path.join(dir, swatch))) {
                opts.push({ id: o.id, name: o.name, position: o.position, note: o.note, page, swatch });
            } else {
                absent.push(o.id);
            }
        }
        if (opts.length < 2) {
            missing.push({ set: set.id, why: `only ${opts.length} rendered option(s)` });
            continue;
        }
        if (absent.length) missing.push({ set: set.id, why: `partial: no PNGs for ${absent.join(', ')}` });
        ready.push({ id: set.id, title: set.title, kind: set.kind, options: opts });
    }
    return { ready, missing };
}

let CATALOG = scanCatalog();
const setOf = (id) => CATALOG.ready.find((s) => s.id === id) || null;

// ---------------------------------------------------------------- judgements

// Held in memory for pair selection, but the file is the source of truth: it
// is reloaded at boot and appended to per judgement, never rewritten.
let JUDGEMENTS = [];

function loadJudgements() {
    JUDGEMENTS = [];
    if (!fs.existsSync(PREFS)) return;
    for (const line of fs.readFileSync(PREFS, 'utf8').split('\n')) {
        const t = line.trim();
        if (!t) continue;
        try {
            const j = JSON.parse(t);
            if (j && j.set && j.a && j.b) JUDGEMENTS.push(j);
        } catch { /* a truncated tail line is not fatal */ }
    }
}

function appendJudgement(row) {
    fs.mkdirSync(OUT_DIR, { recursive: true });
    fs.appendFileSync(PREFS, JSON.stringify(row) + '\n');
    JUDGEMENTS.push(row);
}

const forSet = (id) => JUDGEMENTS.filter((j) => j.set === id);
const pairKey = (a, b) => (a < b ? a + '|' + b : b + '|' + a);

// ---------------------------------------------------------------- pairing

function pickRandom(arr) { return arr[Math.floor(Math.random() * arr.length)]; }

// Fewest judgements relative to target, ties broken randomly. A set already at
// target is still offerable -- it just sorts last.
function chooseSet(requested) {
    if (requested) {
        const s = setOf(requested);
        if (s) return s;
    }
    if (!CATALOG.ready.length) return null;
    const scored = CATALOG.ready.map((s) => ({ s, n: forSet(s.id).length }));
    const min = Math.min(...scored.map((x) => x.n));
    return pickRandom(scored.filter((x) => x.n === min)).s;
}

// Weight toward the least-seen options. Over ~16 draws uniform random pairing
// leaves some options never shown and others shown five times, which is the
// one thing this dataset cannot afford.
function choosePair(set) {
    const seen = new Map(set.options.map((o) => [o.id, 0]));
    const judged = new Set();
    for (const j of forSet(set.id)) {
        if (seen.has(j.a)) seen.set(j.a, seen.get(j.a) + 1);
        if (seen.has(j.b)) seen.set(j.b, seen.get(j.b) + 1);
        judged.add(pairKey(j.a, j.b));
    }
    const count = (o) => seen.get(o.id);
    const leastOf = (pool) => {
        const min = Math.min(...pool.map(count));
        return pickRandom(pool.filter((o) => count(o) === min));
    };

    const a = leastOf(set.options);
    const rest = set.options.filter((o) => o.id !== a.id);
    // Prefer a pair that has not been judged yet; when every pair involving A
    // is exhausted, fall back to the least-seen of the rest rather than
    // searching forever. rest is non-empty because a set needs >= 2 options.
    const fresh = rest.filter((o) => !judged.has(pairKey(a.id, o.id)));
    const b = leastOf(fresh.length ? fresh : rest);

    // Randomise which side each lands on, so position correlates with nothing.
    return Math.random() < 0.5 ? [a, b] : [b, a];
}

function imgUrl(setId, file) { return `/img/${setId}/${file}`; }

function pairPayload(requestedSet) {
    const set = chooseSet(requestedSet);
    if (!set) return { error: 'no rendered sets', missing: CATALOG.missing };
    const [a, b] = choosePair(set);
    const side = (o) => ({ id: o.id, page: imgUrl(set.id, o.page), swatch: imgUrl(set.id, o.swatch) });
    return {
        set: set.id,
        title: set.title,
        judged: forSet(set.id).length,
        target: TARGET_PER_SET,
        a: side(a),
        b: side(b),
    };
}

function revealPayload(setId, aId, bId) {
    const set = setOf(setId);
    if (!set) return null;
    const meta = (id) => {
        const o = set.options.find((x) => x.id === id);
        return o ? { id: o.id, name: o.name, position: o.position, note: o.note } : { id, name: id, position: null, note: '' };
    };
    return { a: meta(aId), b: meta(bId) };
}

function progressPayload() {
    return {
        target: TARGET_PER_SET,
        total: JUDGEMENTS.length,
        sets: CATALOG.ready.map((s) => {
            const rows = forSet(s.id);
            return {
                set: s.id,
                title: s.title,
                options: s.options.length,
                judged: rows.length,
                skipped: rows.filter((r) => r.winner === 'skip').length,
                target: TARGET_PER_SET,
            };
        }),
        missing: CATALOG.missing,
    };
}

// ---------------------------------------------------------------- http

function sendJson(res, code, obj) {
    const body = Buffer.from(JSON.stringify(obj));
    res.writeHead(code, { 'Content-Type': 'application/json', 'Content-Length': body.length, 'Cache-Control': 'no-store' });
    res.end(body);
}

function sendText(res, code, text) {
    const body = Buffer.from(text);
    res.writeHead(code, { 'Content-Type': 'text/plain; charset=utf-8', 'Content-Length': body.length });
    res.end(body);
}

const SAFE_NAME = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;

function serveImage(res, setId, file) {
    // Three independent gates: the set must be one we serve, the filename must
    // be a plain .png name, and the resolved path must still be inside
    // catalog-out. Any one of them alone would do; a traversal bug here would
    // hand out arbitrary files off the developer's disk.
    if (!SAFE_NAME.test(setId) || !setOf(setId)) return sendText(res, 404, 'no such set');
    if (!SAFE_NAME.test(file) || !file.endsWith('.png') || file.includes('..')) return sendText(res, 400, 'bad filename');
    const dir = path.join(OUT_DIR, setId) + path.sep;
    const abs = path.resolve(dir, file);
    if (!abs.startsWith(dir)) return sendText(res, 400, 'bad path');
    let buf;
    try { buf = fs.readFileSync(abs); } catch { return sendText(res, 404, 'not found'); }
    res.writeHead(200, { 'Content-Type': 'image/png', 'Content-Length': buf.length, 'Cache-Control': 'no-store' });
    res.end(buf);
}

function readBody(req) {
    return new Promise((resolve, reject) => {
        const chunks = [];
        let n = 0;
        req.on('data', (c) => {
            n += c.length;
            if (n > 64 * 1024) { reject(new Error('body too large')); req.destroy(); return; }
            chunks.push(c);
        });
        req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
        req.on('error', reject);
    });
}

async function handleJudge(req, res) {
    let body;
    try { body = JSON.parse(await readBody(req) || '{}'); } catch { return sendJson(res, 400, { error: 'bad json' }); }
    const { set, a, b, winner } = body || {};
    const s = setOf(set);
    if (!s) return sendJson(res, 400, { error: 'unknown set' });
    const ids = new Set(s.options.map((o) => o.id));
    if (!ids.has(a) || !ids.has(b) || a === b) return sendJson(res, 400, { error: 'unknown or duplicate option' });
    if (winner !== 'a' && winner !== 'b' && winner !== 'skip') return sendJson(res, 400, { error: 'winner must be a, b or skip' });

    const row = { ts: new Date().toISOString(), set, a, b, winner };
    try { appendJudgement(row); } catch (e) { return sendJson(res, 500, { error: 'write failed: ' + e.message }); }

    sendJson(res, 200, { ok: true, recorded: row, reveal: revealPayload(set, a, b), next: pairPayload(set) });
}

const server = http.createServer((req, res) => {
    let url;
    try { url = new URL(req.url, `http://${HOST}:${PORT}`); } catch { return sendText(res, 400, 'bad url'); }
    const p = url.pathname;

    if (req.method === 'GET' && (p === '/' || p === '/index.html')) {
        const body = Buffer.from(PAGE);
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Content-Length': body.length, 'Cache-Control': 'no-store' });
        return res.end(body);
    }
    if (req.method === 'GET' && p === '/api/pair') {
        CATALOG = scanCatalog();          // pick up sets rendered while running
        return sendJson(res, 200, pairPayload(url.searchParams.get('set')));
    }
    if (req.method === 'GET' && p === '/api/progress') {
        CATALOG = scanCatalog();
        return sendJson(res, 200, progressPayload());
    }
    if (req.method === 'POST' && p === '/api/judge') return handleJudge(req, res);
    if (req.method === 'GET' && p.startsWith('/img/')) {
        const parts = p.slice(5).split('/');
        if (parts.length !== 2) return sendText(res, 400, 'bad image path');
        return serveImage(res, decodeURIComponent(parts[0]), decodeURIComponent(parts[1]));
    }
    return sendText(res, 404, 'not found');
});

// ---------------------------------------------------------------- page

const PAGE = `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>SCH-50 A/B</title>
<style>
:root { color-scheme: dark; }
* { box-sizing: border-box; }
body { margin:0; background:#111; color:#ddd; font:13px/1.45 ui-monospace,SFMono-Regular,Menlo,monospace; }
header { display:flex; gap:14px; align-items:baseline; padding:10px 16px; border-bottom:1px solid #333; flex-wrap:wrap; }
header .title { color:#fff; font-weight:600; }
header .muted { color:#888; }
select { background:#1b1b1b; color:#ddd; border:1px solid #444; padding:3px 6px; font:inherit; }
#bar { height:4px; background:#222; }
#bar div { height:100%; background:#4c8; width:0; transition:width .2s; }
main { display:flex; gap:16px; padding:20px 16px; align-items:flex-start; justify-content:center; }
.card { flex:1 1 0; max-width:calc(50% - 8px); background:#000; border:1px solid #333; padding:10px; text-align:center; cursor:pointer; }
.card:hover { border-color:#777; }
.card img { width:100%; height:auto; image-rendering:pixelated; display:block; background:#000; }
.card .key { margin-top:8px; color:#777; letter-spacing:.08em; }
footer { padding:10px 16px; color:#888; border-top:1px solid #333; display:flex; gap:18px; flex-wrap:wrap; }
#reveal { position:fixed; inset:auto 0 0 0; background:#181818; border-top:2px solid #4c8; padding:14px 16px; display:none; }
#reveal.on { display:block; }
#reveal .cols { display:flex; gap:18px; }
#reveal .col { flex:1 1 0; }
#reveal h3 { margin:0 0 4px; font-size:13px; color:#fff; }
#reveal .pos { color:#4c8; }
#reveal .note { color:#999; font-size:12px; max-height:8.5em; overflow:auto; }
#reveal .win { color:#4c8; }
#reveal .lose { color:#666; }
#msg { padding:20px 16px; color:#c66; }
</style></head><body>
<header>
  <span class="title" id="setTitle">loading…</span>
  <span class="muted" id="count"></span>
  <label class="muted">set <select id="setSel"><option value="">auto (fewest first)</option></select></label>
  <span class="muted" id="mode">in-context</span>
  <span class="muted" id="missing"></span>
</header>
<div id="bar"><div></div></div>
<main>
  <div class="card" id="cardA"><img id="imgA" alt="option A"><div class="key">&#8592; LEFT</div></div>
  <div class="card" id="cardB"><img id="imgB" alt="option B"><div class="key">RIGHT &#8594;</div></div>
</main>
<footer>
  <span>&#8592;/&#8594; pick</span><span>SPACE skip</span><span>S swatch/in-context</span><span>Any key dismisses the reveal</span>
</footer>
<div id="reveal"><div class="cols">
  <div class="col"><h3 id="rnA"></h3><div class="pos" id="rpA"></div><div class="note" id="rtA"></div></div>
  <div class="col"><h3 id="rnB"></h3><div class="pos" id="rpB"></div><div class="note" id="rtB"></div></div>
</div></div>
<div id="msg"></div>
<script>
let cur = null, busy = false, mode = 'page', revealTimer = null;
const $ = (id) => document.getElementById(id);

function show(p) {
  cur = p;
  if (p.error) { $('msg').textContent = p.error + ' — render some sets with tools/param-pages/catalog.mjs first.'; return; }
  $('msg').textContent = '';
  $('setTitle').textContent = p.title;
  $('count').textContent = p.judged + ' / ' + p.target;
  $('bar').firstElementChild.style.width = Math.min(100, 100 * p.judged / p.target) + '%';
  $('imgA').src = p.a[mode]; $('imgB').src = p.b[mode];
  $('mode').textContent = mode === 'page' ? 'in-context' : 'swatch';
}

async function pair(setId) {
  const q = setId ? '?set=' + encodeURIComponent(setId) : '';
  show(await (await fetch('/api/pair' + q)).json());
}

function hideReveal() {
  clearTimeout(revealTimer); revealTimer = null;
  $('reveal').classList.remove('on');
}

function reveal(r, winner, next) {
  const put = (k, o, won) => {
    $('rn' + k).textContent = o.name;
    $('rn' + k).className = won ? 'win' : 'lose';
    $('rp' + k).textContent = 'axis position ' + o.position + (won ? '  \\u2190 chosen' : '');
    $('rt' + k).textContent = o.note || '';
  };
  put('A', r.a, winner === 'a');
  put('B', r.b, winner === 'b');
  $('reveal').classList.add('on');
  revealTimer = setTimeout(() => { hideReveal(); show(next); }, 4500);
  window.__pendingNext = next;
}

async function judge(winner) {
  if (busy || !cur || cur.error) return;
  busy = true;
  try {
    const res = await fetch('/api/judge', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ set: cur.set, a: cur.a.id, b: cur.b.id, winner })
    });
    const j = await res.json();
    if (!res.ok) { $('msg').textContent = j.error || 'judge failed'; return; }
    reveal(j.reveal, winner, j.next);
  } finally { busy = false; }
}

document.addEventListener('keydown', (e) => {
  if ($('reveal').classList.contains('on')) {
    e.preventDefault(); hideReveal();
    if (window.__pendingNext) { show(window.__pendingNext); window.__pendingNext = null; }
    return;
  }
  if (e.key === 'ArrowLeft') { e.preventDefault(); judge('a'); }
  else if (e.key === 'ArrowRight') { e.preventDefault(); judge('b'); }
  else if (e.key === ' ') { e.preventDefault(); judge('skip'); }
  else if (e.key === 's' || e.key === 'S') { mode = mode === 'page' ? 'swatch' : 'page'; if (cur && !cur.error) show(cur); }
});
$('cardA').onclick = () => judge('a');
$('cardB').onclick = () => judge('b');
$('setSel').onchange = (e) => { hideReveal(); pair(e.target.value); };

(async () => {
  const pr = await (await fetch('/api/progress')).json();
  for (const s of pr.sets) {
    const o = document.createElement('option');
    o.value = s.set; o.textContent = s.set + ' (' + s.judged + '/' + s.target + ')';
    $('setSel').appendChild(o);
  }
  if (pr.missing.length) $('missing').textContent = 'not offered: ' + pr.missing.map((m) => m.set + ' — ' + m.why).join('; ');
  await pair('');
})();
</script></body></html>`;

// ---------------------------------------------------------------- boot

loadJudgements();

/* Say it once, loudly, rather than exiting silently into a shell that scrolled
 * away. EADDRINUSE here means something else already owns the port, and the
 * failure mode this guards against is judging against the wrong page. */
server.on('error', (e) => {
    if (e && e.code === 'EADDRINUSE') {
        console.error(`ab_server: port ${PORT} is already in use.`);
        console.error(`  Something else is listening. Pick another: --port ${PORT + 1}`);
        console.error(`  (See the note above the PORT constant -- a browser can silently`);
        console.error(`   reach a DIFFERENT server on a port this one appears to share.)`);
        process.exit(1);
    }
    throw e;
});

server.listen(PORT, HOST, () => {
    const n = CATALOG.ready.length;
    console.log(`A/B comparator on http://${HOST}:${PORT}  (${n} set(s), ${JUDGEMENTS.length} judgement(s) loaded)`);
    console.log(`  Use that address, not localhost -- localhost may resolve to ::1.`);
    for (const m of CATALOG.missing) console.log(`  not offered: ${m.set} — ${m.why}`);
});
