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
import os from 'node:os';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..');
const OUT_DIR = path.join(REPO, 'catalog-out');
const PREFS = path.join(OUT_DIR, 'preferences.json');
const STYLES = path.join(REPO, 'src', 'shared', 'param_pages', 'styles', 'index.mjs');

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

/*
 * Loopback by default; `--lan` opens it to the local network.
 *
 * Opt-in rather than default because there is no auth here at all: anything
 * that can reach the port can read the catalog and, more to the point, POST
 * judgements into the dataset. On a home network that is fine and being able
 * to judge from a phone or a second machine is worth having. On a shared or
 * public network it is not, so it takes a deliberate flag.
 */
let LAN = false;
for (let i = 2; i < process.argv.length; i++) {
    if (process.argv[i] === '--port') PORT = parseInt(process.argv[i + 1], 10) || PORT;
    if (process.argv[i] === '--lan') LAN = true;
}
const HOST = LAN ? '0.0.0.0' : '127.0.0.1';

/** The first non-internal IPv4 address, for printing a reachable URL. */
function lanAddress() {
    const nets = os.networkInterfaces();
    for (const name of Object.keys(nets)) {
        for (const ni of nets[name] || []) {
            if (ni.family === 'IPv4' && !ni.internal) return ni.address;
        }
    }
    return null;
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
        /*
         * A page render is required only where one MEANS anything.
         *
         * A font option and a motion option deliberately have no in-context
         * page: renderPageMovy prints through font4x5 own closed-over table, so
         * ten font substitutions would come back byte-identical; and a page is a
         * still while a motion option IS a sequence. For both, the swatch --
         * the specimen, the strip -- is the judged surface.
         *
         * Demanding both files silently excluded exactly those two sets, and
         * the symptom read as "not rendered yet" rather than as a bug. The two
         * most consequential sets in the catalog were the ones dropped.
         */
        const needsPage = set.kind === "draw";
        for (const o of set.options) {
            const page = `${pad2(o.position)}-${o.id}-page.png`;
            const swatch = `${pad2(o.position)}-${o.id}-swatch.png`;
            const hasSwatch = fs.existsSync(path.join(dir, swatch));
            const hasPage = fs.existsSync(path.join(dir, page));
            if (hasSwatch && (hasPage || !needsPage)) {
                opts.push({ id: o.id, name: o.name, position: o.position, note: o.note,
                            page: hasPage ? page : swatch, swatch });
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

/* Comparisons only. A lock row carries no a/b, so letting it through here would
 * put undefined into every appearance count and pair key downstream. */
const forSet = (id) => JUDGEMENTS.filter((j) => j.set === id && !j.lock);
const lockFor = (id) => {
    const rows = JUDGEMENTS.filter((j) => j.set === id && j.lock);
    return rows.length ? rows[rows.length - 1].lock : null;
};
const pairKey = (a, b) => (a < b ? a + '|' + b : b + '|' + a);

// ---------------------------------------------------------------- pairing

function pickRandom(arr) { return arr[Math.floor(Math.random() * arr.length)]; }

/*
 * Fewest judgements first, ties broken randomly.
 *
 * Locked sets are OUT: the decision is made, so asking again spends judgements
 * that another set needs. Sets at or over target are also out while any set is
 * still short — otherwise a session tops up one set past its target while
 * others sit at zero, which is exactly what happened when the judge handler
 * fed its own set back in as the request. Only once every set is done or
 * locked does the pool reopen, so a long session can keep going rather than
 * dead-ending.
 *
 * An explicit pin always wins, including on a locked set: asking for one by
 * name is a deliberate act.
 */
function chooseSet(requested) {
    if (requested) {
        const s = setOf(requested);
        if (s) return s;
    }
    if (!CATALOG.ready.length) return null;
    const open = CATALOG.ready.filter((s) => !lockFor(s.id));
    if (!open.length) return null;
    const scored = open.map((s) => ({ s, n: forSet(s.id).length }));
    const short = scored.filter((x) => x.n < TARGET_PER_SET);
    const pool = short.length ? short : scored;
    const min = Math.min(...pool.map((x) => x.n));
    return pickRandom(pool.filter((x) => x.n === min)).s;
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
    if (!set) {
        const anyOpen = CATALOG.ready.some((s) => !lockFor(s.id));
        return anyOpen
            ? { error: 'no rendered sets', missing: CATALOG.missing }
            : { error: 'every set is locked — nothing left to judge', done: true };
    }
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
        build: BUILD,
        target: TARGET_PER_SET,
        total: JUDGEMENTS.filter((j) => !j.lock).length,
        sets: CATALOG.ready.map((s) => {
            const rows = forSet(s.id);
            return {
                set: s.id,
                title: s.title,
                options: s.options.length,
                judged: rows.length,
                skipped: rows.filter((r) => r.winner === 'skip').length,
                target: TARGET_PER_SET,
                lock: lockFor(s.id),
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
    const { set, a, b, winner, pin } = body || {};
    const s = setOf(set);
    if (!s) return sendJson(res, 400, { error: 'unknown set' });
    const ids = new Set(s.options.map((o) => o.id));
    if (!ids.has(a) || !ids.has(b) || a === b) return sendJson(res, 400, { error: 'unknown or duplicate option' });
    if (winner !== 'a' && winner !== 'b' && winner !== 'skip') return sendJson(res, 400, { error: 'winner must be a, b or skip' });

    const row = { ts: new Date().toISOString(), set, a, b, winner };
    try { appendJudgement(row); } catch (e) { return sendJson(res, 500, { error: 'write failed: ' + e.message }); }

    /*
     * `pin`, not `set`.
     *
     * This used to hand the set just judged straight back as the requested set,
     * so the session never rotated: one set ran past its target forever while
     * twelve others stayed at zero. Passing the user pin (empty when the picker
     * is on auto) lets chooseSet do the job it was written for.
     */
    sendJson(res, 200, {
        ok: true, recorded: row,
        reveal: revealPayload(set, a, b),
        next: pairPayload(pin || null),
    });
}

/*
 * Lock a set: this option wins, stop asking about it.
 *
 * When the answer is already obvious there is no reason to spend a dozen more
 * comparisons confirming it, and a session that keeps asking after the decision
 * is made is a session that gets abandoned. A lock is recorded as its own row
 * so the ranking can report it as a DECISION rather than silently folding it
 * into strengths derived from far fewer comparisons.
 */
async function handleLock(req, res) {
    let body;
    try { body = JSON.parse(await readBody(req) || '{}'); } catch { return sendJson(res, 400, { error: 'bad json' }); }
    const { set, id, pin } = body || {};
    const s = setOf(set);
    if (!s) return sendJson(res, 400, { error: 'unknown set' });
    if (!s.options.some((o) => o.id === id)) return sendJson(res, 400, { error: 'unknown option' });

    const row = { ts: new Date().toISOString(), set, lock: id };
    try { appendJudgement(row); } catch (e) { return sendJson(res, 500, { error: 'write failed: ' + e.message }); }

    sendJson(res, 200, { ok: true, recorded: row, locked: id, next: pairPayload(pin || null) });
}

const server = http.createServer((req, res) => {
    let url;
    try { url = new URL(req.url, `http://${HOST}:${PORT}`); } catch { return sendText(res, 400, 'bad url'); }
    const p = url.pathname;

    if (req.method === 'GET' && (p === '/' || p === '/index.html')) {
        const body = Buffer.from(PAGE.replace('__BUILD__', BUILD));
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
    if (req.method === 'POST' && p === '/api/lock') return handleLock(req, res);
    if (req.method === 'GET' && p.startsWith('/img/')) {
        const parts = p.slice(5).split('/');
        if (parts.length !== 2) return sendText(res, 400, 'bad image path');
        return serveImage(res, decodeURIComponent(parts[0]), decodeURIComponent(parts[1]));
    }
    return sendText(res, 404, 'not found');
});

// ---------------------------------------------------------------- page

/*
 * A build stamp, so "have you reloaded?" is answerable by looking.
 *
 * The page sets no-store, but that does not reload a tab that is already open,
 * and a tab left open across a server restart keeps running the old client
 * against the new API. That cost a round of debugging: the server was rotating
 * between sets correctly while the browser sat on a stale count, and neither
 * side looked wrong on its own. The client polls this and says so when it
 * drifts.
 */
const BUILD = String(Date.now());

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
/* The reveal is a LOG of the judgement just made, not a gate in front of the
 * next one. It never blocks: the next pair is already on screen above it.
 * Fixed height so the cards do not jump when it first appears. */
#reveal { border-top:1px solid #333; background:#161616; padding:8px 16px; min-height:74px; }
#reveal .lead { color:#555; font-size:11px; letter-spacing:.08em; margin-bottom:4px; }
#reveal .cols { display:flex; gap:18px; }
#reveal .col { flex:1 1 0; }
#reveal h3 { margin:0 0 2px; font-size:12px; color:#fff; }
#reveal .pos { color:#4c8; font-size:11px; }
#reveal .note { color:#888; font-size:11px; max-height:3.2em; overflow:auto; }
#reveal .win { color:#4c8; }
#reveal .lose { color:#666; }
#msg { padding:20px 16px; color:#c66; }
</style></head><body>
<header>
  <span class="title" id="setTitle">loading…</span>
  <span class="muted" id="count"></span>
  <label class="muted">set <select id="setSel"></select></label>
  <span class="muted" id="mode">in-context</span>
  <span class="muted" id="totals"></span>
  <span class="muted" id="missing"></span>
</header>
<div id="bar"><div></div></div>
<main>
  <div class="card" id="cardA"><img id="imgA" alt="option A"><div class="key">&#8592; LEFT</div></div>
  <div class="card" id="cardB"><img id="imgB" alt="option B"><div class="key">RIGHT &#8594;</div></div>
</main>
<footer>
  <span>&#8592;/&#8594; pick</span><span>SPACE skip</span><span>S swatch/in-context</span>
  <span style="color:#4c8">SHIFT+&#8592;/&#8594; lock this one and close the set</span>
</footer>
<div id="reveal"><div class="lead" id="rlead">what you just picked appears here</div><div class="cols">
  <div class="col"><h3 id="rnA"></h3><div class="pos" id="rpA"></div><div class="note" id="rtA"></div></div>
  <div class="col"><h3 id="rnB"></h3><div class="pos" id="rpB"></div><div class="note" id="rtB"></div></div>
</div></div>
<div id="msg"></div>
<script>
let cur = null, busy = false, mode = 'page';
const MY_BUILD = '__BUILD__';
const $ = (id) => document.getElementById(id);

function show(p) {
  cur = p;
  if (p.error) { $('msg').textContent = p.error + ' — render some sets with tools/param-pages/catalog.mjs first.'; return; }
  $('msg').textContent = '';
  $('setTitle').textContent = p.title;
  /* Over target is legitimate but only ever because a set is PINNED -- on auto
   * a set at target drops out of rotation. Saying which it is turns a number
   * that looks broken into one that explains itself. */
  const over = p.judged >= p.target;
  $('count').textContent = p.judged + ' / ' + p.target
    + (over ? ($('setSel').value ? '  (pinned, past target)' : '  (past target)') : '');
  $('count').style.color = over ? '#c93' : '';
  $('bar').firstElementChild.style.width = Math.min(100, 100 * p.judged / p.target) + '%';
  $('bar').firstElementChild.style.background = over ? '#c93' : '#4c8';
  $('imgA').src = p.a[mode]; $('imgB').src = p.b[mode];
  $('mode').textContent = mode === 'page' ? 'in-context' : 'swatch';
  refreshTotals();
}

/* Global progress, so the header is never the only signal. A single set count
 * cannot distinguish "this set is done" from "the session is stuck on it". */
let totalsBusy = false;
async function refreshTotals() {
  if (totalsBusy) return;
  totalsBusy = true;
  try {
    const pr = await (await fetch('/api/progress')).json();
    const done = pr.sets.filter((s) => s.lock || s.judged >= s.target).length;
    $('totals').textContent = pr.total + ' judgement(s) | ' + done + '/' + pr.sets.length + ' set(s) done';
    if (pr.build && pr.build !== MY_BUILD) {
      $('msg').innerHTML = '<b style="color:#fc6">This page is stale.</b> The server restarted since it was loaded '
        + '— reload (Cmd-Shift-R) before judging, or you are driving an old client against a new API.';
    }
  } finally { totalsBusy = false; }
}

async function pair(setId) {
  const q = setId ? '?set=' + encodeURIComponent(setId) : '';
  show(await (await fetch('/api/pair' + q)).json());
}

/*
 * Populate the log strip. This NEVER gates the next pair.
 *
 * It used to: a 4500ms timer held the reveal up and only then advanced. Over a
 * two hundred judgement session that is fifteen minutes of sitting still, and
 * it made a fast pass impossible even though the reveal is informational --
 * you are told what you already chose. Now the next pair is on screen before
 * this runs, and the strip simply reports the previous one.
 */
function reveal(r, winner) {
  const put = (k, o, won) => {
    $('rn' + k).textContent = o.name;
    $('rn' + k).className = won ? 'win' : 'lose';
    $('rp' + k).textContent = 'axis position ' + o.position + (won ? '  \\u2190 chosen' : '');
    $('rt' + k).textContent = o.note || '';
  };
  put('A', r.a, winner === 'a');
  put('B', r.b, winner === 'b');
  $('rlead').textContent = winner === 'skip' ? 'SKIPPED - recorded as too close to call' : 'you picked:';
}

async function judge(winner) {
  if (busy || !cur || cur.error) return;
  busy = true;
  const asked = cur;
  try {
    const res = await fetch('/api/judge', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ set: asked.set, a: asked.a.id, b: asked.b.id, winner,
                             pin: $('setSel').value || '' })
    });
    const j = await res.json();
    if (!res.ok) { $('msg').textContent = j.error || 'judge failed'; return; }
    /* Next pair first, so the wait is the round trip and nothing else. */
    show(j.next);
    reveal(j.reveal, winner);
  } finally { busy = false; }
}

/* Declare a winner and drop the set out of rotation. When the answer is
 * already obvious, another dozen comparisons confirming it is time taken from
 * the twelve sets that still need it. */
async function lockIn(side) {
  if (busy || !cur || cur.error) return;
  busy = true;
  const asked = cur;
  const chosen = side === 'a' ? asked.a.id : asked.b.id;
  try {
    const res = await fetch('/api/lock', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ set: asked.set, id: chosen, pin: $('setSel').value || '' })
    });
    const j = await res.json();
    if (!res.ok) { $('msg').textContent = j.error || 'lock failed'; return; }
    show(j.next);
    $('rlead').textContent = 'LOCKED: ' + chosen + ' wins ' + asked.set + ' — set closed';
    $('rnA').textContent = ''; $('rpA').textContent = ''; $('rtA').textContent = '';
    $('rnB').textContent = ''; $('rpB').textContent = ''; $('rtB').textContent = '';
    await refreshSets();
  } finally { busy = false; }
}

/* No dismiss branch here any more. While the reveal was a blocking panel, the
 * first key after every judgement was eaten dismissing it -- so a fast run of
 * arrow presses silently dropped every other one. The strip is passive now, so
 * every key goes straight to a judgement. */
document.addEventListener('keydown', (e) => {
  if (e.shiftKey && e.key === 'ArrowLeft') { e.preventDefault(); lockIn('a'); return; }
  if (e.shiftKey && e.key === 'ArrowRight') { e.preventDefault(); lockIn('b'); return; }
  if (e.key === 'ArrowLeft') { e.preventDefault(); judge('a'); }
  else if (e.key === 'ArrowRight') { e.preventDefault(); judge('b'); }
  else if (e.key === ' ') { e.preventDefault(); judge('skip'); }
  else if (e.key === 's' || e.key === 'S') { mode = mode === 'page' ? 'swatch' : 'page'; if (cur && !cur.error) show(cur); }
});
$('cardA').onclick = () => judge('a');
$('cardB').onclick = () => judge('b');
$('setSel').onchange = (e) => pair(e.target.value);

/* Rebuilt after a lock so the picker shows what is closed. Keeps the current
 * selection if it still exists, so pinning a set survives the refresh. */
async function refreshSets() {
  const pr = await (await fetch('/api/progress')).json();
  const keep = $('setSel').value;
  $('setSel').innerHTML = '';
  const auto = document.createElement('option');
  auto.value = ''; auto.textContent = 'auto (fewest first)';
  $('setSel').appendChild(auto);
  for (const s of pr.sets) {
    const o = document.createElement('option');
    o.value = s.set;
    o.textContent = s.set + (s.lock ? '  LOCKED: ' + s.lock : ' (' + s.judged + '/' + s.target + ')');
    $('setSel').appendChild(o);
  }
  $('setSel').value = keep;
  if (pr.missing.length) $('missing').textContent = 'not offered: ' + pr.missing.map((m) => m.set + ' — ' + m.why).join('; ');
  return pr;
}

(async () => {
  await refreshSets();
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
    const lan = LAN ? lanAddress() : null;
    console.log(`A/B comparator  (${n} set(s), ${JUDGEMENTS.length} judgement(s) loaded)`);
    console.log(`  http://127.0.0.1:${PORT}`);
    if (LAN) {
        if (lan) console.log(`  http://${lan}:${PORT}   <- this machine on the LAN`);
        else console.log(`  bound to 0.0.0.0 but no external IPv4 address was found`);
        console.log(`  Open to the local network and there is NO auth: anyone who can`);
        console.log(`  reach this port can post judgements into the dataset.`);
    } else {
        console.log(`  Loopback only. Pass --lan to reach it from another device.`);
    }
    console.log(`  Use the numeric address, not localhost -- localhost may resolve to ::1.`);
    for (const m of CATALOG.missing) console.log(`  not offered: ${m.set} — ${m.why}`);
});
