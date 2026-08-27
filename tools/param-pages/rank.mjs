#!/usr/bin/env node
// Bradley-Terry ranking over the SCH-50 pairwise judgement log.
//
//   node tools/param-pages/rank.mjs [--json] [--file catalog-out/preferences.json]
//
// tools/param-pages/ab_server.mjs appends one JSON object per judgement:
//
//   {"ts":"...","set":"knob","a":"thin-arc","b":"dotted-track","winner":"b"}
//
// The target is ~16 judgements per set. A complete ordering of ten options
// would need all 45 pairs, so a tally cannot be the answer: most pairs are
// never seen at all, and the ones that are are seen once. Bradley-Terry infers
// a single strength per option from whatever overlapping comparisons exist,
// which is what makes 16 judgements say anything about 10 options.
//
// Two decisions worth stating plainly, because both change what the numbers
// mean:
//
//   SKIPS ARE NOT EVIDENCE. A skip says the pair was indistinguishable. Fed to
//   the fit as a half-win each it would pull both options toward the average
//   and read as "these are equal", which is a claim the judge declined to
//   make. They are counted and reported instead -- a set with many skips is
//   telling you its OPTIONS are too alike, which is a finding about the set.
//
//   THE PRIOR IS HALF A WIN AND HALF A LOSS against a phantom opponent held at
//   the average. Without it an option that lost every comparison has maximum
//   likelihood at strength zero (log-strength -infinity) and the fit does not
//   converge. At ~16 judgements over ten options, options with one or two
//   comparisons are the normal case, not the edge case, so this is load-
//   bearing rather than a numerical nicety. It also shrinks a 1-0 record
//   toward the average, which is the honest reading of one comparison.
//
// The intervals come from the inverse Fisher information of the penalised
// log-likelihood, with the phantom opponent treated as a FIXED reference at
// log-strength 0. That both fixes the model's arbitrary additive constant and
// keeps the information matrix invertible when the comparison graph is
// disconnected -- which at this sample size it usually is.
//
// Dependency-free: node:fs, node:path, node:url only.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..');
const DEFAULT_PREFS = path.join(REPO, 'catalog-out', 'preferences.json');
const STYLES = path.join(REPO, 'src', 'shared', 'param_pages', 'styles', 'index.mjs');

/* Tuning. All three are reported in the output so a reader is never guessing. */
export const PRIOR = 0.5;            /* half a win and half a loss vs the phantom */
export const MAX_ITER = 500;
export const TOL = 1e-10;            /* max |delta log-strength| between iterations */
export const Z = 1.959964;           /* 95% normal quantile */

/* Below this, a per-set ranking is decoration. 10 options need ~45 pairs for a
 * complete ordering; 2x the option count is the point where most options have
 * been seen at least twice and the ordering starts to mean something. */
export const THIN_PER_OPTION = 2;
export const SKIPPY_FRACTION = 0.25; /* skips at or above this share => say so */

/* ------------------------------------------------------------------ fit -- */

/**
 * Bradley-Terry MM fit over rows for ONE set.
 *
 * @param rows  [{a, b, winner}]; winner is "a" | "b" | "skip". Any other value
 *              (including a truncated line that parsed) is ignored, not
 *              guessed at. A `set` field is permitted and unused -- the caller
 *              groups.
 * @returns array sorted strongest first, each:
 *          { id, strength, logStrength, lo, hi, se, wins, losses, n, opponents,
 *            rank }
 *          lo/hi are the 95% interval on the LOG scale. An empty input gives
 *          an empty array.
 */
export function fit(rows) {
    const ids = [];
    const index = new Map();
    const idOf = (id) => {
        if (!index.has(id)) { index.set(id, ids.length); ids.push(id); }
        return index.get(id);
    };

    /* Two passes: register every id that appears in a FITTABLE row, so a pair
     * that was only ever skipped does not conjure options into the ranking.
     * An option judged solely by skips has no evidence about it whatsoever;
     * listing it at the prior would look like a measurement. */
    const fittable = [];
    for (const r of rows || []) {
        if (!r || !r.a || !r.b || r.a === r.b) continue;
        if (r.winner !== 'a' && r.winner !== 'b') continue;
        fittable.push(r);
    }
    for (const r of fittable) { idOf(r.a); idOf(r.b); }

    const n = ids.length;
    if (n === 0) return [];

    const wins = new Float64Array(n);
    const losses = new Float64Array(n);
    /* n_ij: comparison counts, symmetric, dense (n <= 10 here). */
    const cnt = Array.from({ length: n }, () => new Float64Array(n));

    for (const r of fittable) {
        const i = index.get(r.a);
        const j = index.get(r.b);
        cnt[i][j] += 1;
        cnt[j][i] += 1;
        if (r.winner === 'a') { wins[i] += 1; losses[j] += 1; }
        else { wins[j] += 1; losses[i] += 1; }
    }

    /* MM iteration on the strength scale. p_i <- (w_i + PRIOR) / D_i where
     *
     *   D_i = sum_{j != i} n_ij / (p_i + p_j)  +  2*PRIOR / (p_i + 1)
     *
     * The second term is the phantom: PRIOR wins and PRIOR losses against an
     * opponent pinned at strength 1, i.e. 2*PRIOR comparisons. Normalising by
     * the geometric mean each round keeps 1 meaning "the average option", so
     * the phantom stays where it claims to be. */
    let p = new Float64Array(n).fill(1);
    let iterations = 0;
    let converged = false;
    for (; iterations < MAX_ITER; iterations++) {
        const next = new Float64Array(n);
        for (let i = 0; i < n; i++) {
            let d = 2 * PRIOR / (p[i] + 1);
            for (let j = 0; j < n; j++) {
                if (j === i || cnt[i][j] === 0) continue;
                d += cnt[i][j] / (p[i] + p[j]);
            }
            next[i] = d > 0 ? (wins[i] + PRIOR) / d : p[i];
            if (!(next[i] > 0) || !Number.isFinite(next[i])) next[i] = 1e-12;
        }
        /* Geometric-mean normalisation. */
        let logSum = 0;
        for (let i = 0; i < n; i++) logSum += Math.log(next[i]);
        const g = Math.exp(logSum / n);
        for (let i = 0; i < n; i++) next[i] /= g;

        let delta = 0;
        for (let i = 0; i < n; i++) delta = Math.max(delta, Math.abs(Math.log(next[i]) - Math.log(p[i])));
        p = next;
        if (delta < TOL) { converged = true; iterations++; break; }
    }

    /* Fisher information for beta = log p, phantom FIXED at beta = 0.
     *
     *   I_ii = sum_{j != i} n_ij * p_i p_j / (p_i + p_j)^2  +  2*PRIOR * p_i / (p_i + 1)^2
     *   I_ij = -n_ij * p_i p_j / (p_i + p_j)^2
     *
     * The phantom term is what makes this invertible: with 16 judgements over
     * 10 options the comparison graph routinely has isolated components, and
     * the unpenalised information matrix is singular for every one of them. */
    const I = Array.from({ length: n }, () => new Float64Array(n));
    for (let i = 0; i < n; i++) {
        I[i][i] = 2 * PRIOR * p[i] / ((p[i] + 1) * (p[i] + 1));
        for (let j = 0; j < n; j++) {
            if (j === i || cnt[i][j] === 0) continue;
            const s = p[i] + p[j];
            const v = cnt[i][j] * p[i] * p[j] / (s * s);
            I[i][i] += v;
            I[i][j] -= v;
        }
    }
    const cov = invertSymmetric(I);

    const out = ids.map((id, i) => {
        let opponents = 0;
        for (let j = 0; j < n; j++) if (j !== i && cnt[i][j] > 0) opponents++;
        const varI = cov ? cov[i][i] : NaN;
        const se = varI > 0 ? Math.sqrt(varI) : NaN;
        const beta = Math.log(p[i]);
        return {
            id,
            strength: p[i],
            logStrength: beta,
            se,
            lo: Number.isFinite(se) ? beta - Z * se : -Infinity,
            hi: Number.isFinite(se) ? beta + Z * se : Infinity,
            wins: wins[i],
            losses: losses[i],
            n: wins[i] + losses[i],
            opponents,
        };
    });

    /* Strongest first. Ties broken by id so the order is stable across runs --
     * an unstable order would make two runs of the same data look like new
     * information. */
    out.sort((x, y) => (y.logStrength - x.logStrength) || (x.id < y.id ? -1 : x.id > y.id ? 1 : 0));
    out.forEach((o, k) => { o.rank = k + 1; });
    out.converged = converged;
    out.iterations = iterations;
    return out;
}

/** Gauss-Jordan with partial pivoting. Returns null if singular. */
function invertSymmetric(m) {
    const n = m.length;
    if (n === 0) return [];
    const a = m.map((row, i) => {
        const r = new Float64Array(2 * n);
        for (let j = 0; j < n; j++) r[j] = row[j];
        r[n + i] = 1;
        return r;
    });
    for (let c = 0; c < n; c++) {
        let piv = c;
        for (let r = c + 1; r < n; r++) if (Math.abs(a[r][c]) > Math.abs(a[piv][c])) piv = r;
        if (!(Math.abs(a[piv][c]) > 1e-12)) return null;
        if (piv !== c) { const t = a[piv]; a[piv] = a[c]; a[c] = t; }
        const d = a[c][c];
        for (let j = 0; j < 2 * n; j++) a[c][j] /= d;
        for (let r = 0; r < n; r++) {
            if (r === c) continue;
            const f = a[r][c];
            if (f === 0) continue;
            for (let j = 0; j < 2 * n; j++) a[r][j] -= f * a[c][j];
        }
    }
    return a.map((r) => Array.from(r.slice(n)));
}

/* ------------------------------------------------------------ analysis -- */

/** Spearman rho between two equal-length numeric arrays. Null if n < 3. */
export function spearman(xs, ys) {
    const n = xs.length;
    if (n < 3 || ys.length !== n) return null;
    const rank = (v) => {
        const order = v.map((x, i) => [x, i]).sort((p, q) => p[0] - q[0]);
        const r = new Array(n);
        let i = 0;
        while (i < n) {
            let j = i;
            while (j + 1 < n && order[j + 1][0] === order[i][0]) j++;
            const avg = (i + j) / 2 + 1;
            for (let k = i; k <= j; k++) r[order[k][1]] = avg;
            i = j + 1;
        }
        return r;
    };
    const rx = rank(xs);
    const ry = rank(ys);
    const mean = (a) => a.reduce((s, x) => s + x, 0) / n;
    const mx = mean(rx);
    const my = mean(ry);
    let num = 0, dx = 0, dy = 0;
    for (let i = 0; i < n; i++) {
        num += (rx[i] - mx) * (ry[i] - my);
        dx += (rx[i] - mx) ** 2;
        dy += (ry[i] - my) ** 2;
    }
    if (dx === 0 || dy === 0) return null;
    return num / Math.sqrt(dx * dy);
}

/**
 * Everything worth saying about one set: the fit, the axis comparison, the
 * skip accounting, and the caveats. Kept separate from printing so --json and
 * the text report cannot drift.
 *
 * @param setMeta { id, title, options:[{id,name,position}] } or null if the
 *                set is in the log but not in the registry.
 */
export function analyseSet(setId, rows, setMeta) {
    /*
     * A LOCK is a decision, not evidence.
     *
     * The comparator lets a set be closed outright once the answer is obvious
     * ("this one wins, stop asking"). That row carries no a/b, so it must not
     * reach the fit — and it must not be quietly folded into strengths either,
     * because a declared winner and a winner inferred from twelve comparisons
     * are different claims and the reader is entitled to know which this is.
     * Reported separately, alongside whatever comparisons were made first.
     */
    const locks = rows.filter((r) => r.lock);
    const lock = locks.length ? locks[locks.length - 1].lock : null;
    const judged = rows.filter((r) => r.winner === 'a' || r.winner === 'b');
    const skipped = rows.filter((r) => r.winner === 'skip');
    const known = new Map((setMeta && setMeta.options || []).map((o) => [o.id, o]));

    /* fit() carries `converged` / `iterations` as properties on the returned
     * array, and .map() drops them, so read them off the fit BEFORE mapping.
     * Losing them silently would report every fit as converged. */
    const raw = fit(judged);
    const converged = raw.converged !== false;
    const iterations = raw.iterations || 0;

    const ranked = raw.map((f) => {
        const meta = known.get(f.id) || null;
        return {
            ...f,
            name: meta ? meta.name : f.id,
            position: meta ? meta.position : null,
            unknown: !meta && known.size > 0,
        };
    });

    const rankedIds = new Set(ranked.map((r) => r.id));
    const unjudged = (setMeta && setMeta.options || [])
        .filter((o) => !rankedIds.has(o.id))
        .map((o) => ({ id: o.id, name: o.name, position: o.position }));

    /* Axis agreement, over the options the registry knows a position for. */
    const withPos = ranked.filter((r) => Number.isInteger(r.position));
    const rho = spearman(withPos.map((r) => r.position), withPos.map((r) => r.rank));
    const movers = withPos
        .map((r) => ({ id: r.id, name: r.name, position: r.position, rank: r.rank, moved: Math.abs(r.position - r.rank) }))
        .filter((m) => m.moved > 3)
        .sort((x, y) => y.moved - x.moved);

    const optionCount = known.size || ranked.length;
    const total = rows.length;
    const skipFraction = total ? skipped.length / total : 0;

    /* "Not separated from the leader": the 95% intervals overlap, so the data
     * does not distinguish them. On thin data this is most of the table, which
     * is the point -- it is the column that stops a reader treating rank 1 as
     * a winner. */
    const top = ranked[0] || null;
    for (const r of ranked) r.separatedFromTop = !!top && r !== top && r.lo > top.hi;

    const caveats = [];
    if (judged.length === 0) caveats.push('no judgements yet -- nothing to rank');
    else if (optionCount && judged.length < THIN_PER_OPTION * optionCount)
        caveats.push('THIN: ' + judged.length + ' judgements over ' + optionCount + ' options (want at least '
            + THIN_PER_OPTION * optionCount + '). Treat this ordering as a sketch, not a result.');
    if (unjudged.length) caveats.push(unjudged.length + ' option(s) have no judgements at all and are not ranked');
    if (skipped.length && skipFraction >= SKIPPY_FRACTION)
        caveats.push('SKIPS ARE ' + Math.round(skipFraction * 100) + '% OF THIS SET. That is a result: these options '
            + 'are hard to tell apart, so the ranking below is separating things the judge could not.');
    const separated = ranked.filter((r) => r.separatedFromTop).length;
    if (ranked.length > 1 && separated === 0)
        caveats.push('no option is separated from rank 1 at 95% confidence -- every interval overlaps the leader');
    if (ranked.some((r) => r.unknown))
        caveats.push('option id(s) in the log are not in the registry (renamed after judging?): '
            + ranked.filter((r) => r.unknown).map((r) => r.id).join(', '));
    if (!setMeta) caveats.push('set "' + setId + '" is in the log but not in the registry -- no axis positions, no names');
    if (ranked.length && !converged)
        caveats.push('fit hit the ' + MAX_ITER + '-iteration cap without converging to ' + TOL);

    if (lock) {
        const name = known.has(lock) ? known.get(lock).name : lock;
        caveats.unshift('LOCKED: "' + name + '" was declared the winner and the set was closed. '
            + (judged.length
                ? 'The ' + judged.length + ' comparison(s) below were made before that and are shown for context, '
                  + 'but the decision is the lock, not the fit.'
                : 'No comparisons were made, so there is no ranking here at all -- only the decision.'));
    }

    return {
        set: setId,
        title: setMeta ? setMeta.title : null,
        lock,
        judgements: judged.length,
        skips: skipped.length,
        skipFraction,
        total,
        optionCount,
        iterations,
        converged,
        spearman: rho,
        movers,
        ranked,
        unjudged,
        caveats,
    };
}

/* ------------------------------------------------------------ the CLI --- */

export function loadRows(file) {
    if (!fs.existsSync(file)) return null;
    const rows = [];
    for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
        const t = line.trim();
        if (!t) continue;
        try {
            const j = JSON.parse(t);
            /* Two row shapes: a comparison {set,a,b,winner} and a lock
             * {set,lock}. Requiring a/b silently dropped every lock, so a set
             * the judge had DECIDED reported as "no judgements yet -- nothing
             * to rank", which reads as untouched rather than as settled. */
            if (!j || !j.set) continue;
            if (j.lock || (j.a && j.b)) rows.push(j);
        } catch { /* a truncated tail line is not fatal */ }
    }
    return rows;
}

const pad = (s, w) => String(s).padEnd(w).slice(0, w);
const lpad = (s, w) => String(s).padStart(w);
const sgn = (x) => (x >= 0 ? '+' : '') + x.toFixed(2);

function printSet(a) {
    const title = a.title ? a.title + '  (' + a.set + ')' : a.set;
    console.log('');
    console.log('=== ' + title + ' ' + '='.repeat(Math.max(0, 66 - title.length)));
    console.log('    ' + a.judgements + ' judgement(s), ' + a.skips + ' skip(s)'
        + (a.total ? '  [' + Math.round(a.skipFraction * 100) + '% skipped]' : ''));

    if (!a.ranked.length) {
        for (const c of a.caveats) console.log('    ! ' + c);
        return;
    }

    console.log('');
    console.log('    ' + pad('#', 3) + pad('option', 30) + lpad('axis', 5) + lpad('move', 6)
        + lpad('log-str', 9) + '  ' + pad('95% interval', 18) + lpad('W-L', 7) + lpad('vs', 4));
    console.log('    ' + '-'.repeat(82));
    for (const r of a.ranked) {
        const axis = Number.isInteger(r.position) ? String(r.position) : (r.unknown ? '?' : '-');
        const move = Number.isInteger(r.position) ? (r.position - r.rank === 0 ? '.' : (r.position - r.rank > 0 ? '+' : '') + (r.position - r.rank)) : '';
        const iv = Number.isFinite(r.se) ? '[' + sgn(r.lo) + ', ' + sgn(r.hi) + ']' : '[undetermined]';
        const label = r.name === r.id ? r.id + (r.unknown ? ' (not in registry)' : '') : r.name + ' (' + r.id + ')';
        console.log('    ' + pad(r.rank, 3) + pad(label, 30)
            + lpad(axis, 5) + lpad(move, 6) + lpad(sgn(r.logStrength), 9) + '  ' + pad(iv, 18)
            + lpad(r.wins + '-' + r.losses, 7) + lpad(r.opponents, 4));
    }
    console.log('    (log-str: log strength, 0 = the average option. "vs" = distinct opponents seen.)');

    console.log('');
    const positioned = a.ranked.filter((r) => Number.isInteger(r.position)).length;
    if (a.spearman === null) console.log('    axis agreement: too few positioned options to correlate');
    else {
        const rho = a.spearman;
        const read = rho > 0.6 ? 'the authored minimal->radical axis largely predicts preference'
            : rho < -0.6 ? 'preference runs OPPOSITE to the authored axis'
            : Math.abs(rho) < 0.3 ? 'the authored axis does not predict preference'
            : 'a weak lean along the authored axis, not enough at this sample size to call';
        console.log('    axis agreement: Spearman rho = ' + rho.toFixed(2) + '  -- ' + read);
    }
    if (a.movers.length) {
        console.log('    moved more than 3 places:');
        for (const m of a.movers)
            console.log('      ' + pad(m.name, 26) + 'axis ' + m.position + ' -> rank ' + m.rank
                + '  (' + m.moved + ' places ' + (m.rank < m.position ? 'up' : 'down') + ')');
    } else if (positioned) {
        console.log('    no option moved more than 3 places from its authored position');
    }

    if (a.unjudged.length)
        console.log('    not judged at all: ' + a.unjudged.map((o) => o.name || o.id).join(', '));

    if (a.caveats.length) {
        console.log('');
        for (const c of a.caveats) console.log('    ! ' + c);
    }
}

async function main() {
    const argv = process.argv.slice(2);
    const asJson = argv.includes('--json');
    const fi = argv.indexOf('--file');
    const file = fi >= 0 && argv[fi + 1] ? path.resolve(argv[fi + 1]) : DEFAULT_PREFS;

    const rows = loadRows(file);
    if (rows === null) {
        if (asJson) console.log(JSON.stringify({ file, error: 'no judgement log', sets: [] }, null, 2));
        else {
            console.log('No judgement log at ' + file);
            console.log('Nothing to rank yet. Collect preferences with:');
            console.log('  node tools/param-pages/ab_server.mjs');
        }
        return;
    }

    /* The registry is optional: without it there are no names and no axis, but
     * the ranking still stands. A broken styles/index.mjs must not take the
     * ranking of already-collected data down with it. */
    let SETS = [];
    try { SETS = (await import(pathToFileURL(STYLES).href)).SETS || []; }
    catch (e) { if (!asJson) console.log('(styles registry unavailable: ' + (e && e.message) + ')'); }
    const metaById = new Map(SETS.map((s) => [s.id, s]));

    /* EVERY registry set, in registry order, judged or not -- a set with zero
     * judgements is a thing the reader needs to see, not a row to omit. Then
     * any set that exists only in the log. */
    const inLog = [...new Set(rows.map((r) => r.set))];
    const order = SETS.map((s) => s.id).concat(inLog.filter((id) => !metaById.has(id)));

    const analyses = order.map((id) => analyseSet(id, rows.filter((r) => r.set === id), metaById.get(id) || null));

    if (asJson) { console.log(JSON.stringify({ file, prior: PRIOR, z: Z, sets: analyses }, null, 2)); return; }

    const judged = analyses.reduce((s, a) => s + a.judgements, 0);
    const skips = analyses.reduce((s, a) => s + a.skips, 0);
    console.log('Bradley-Terry ranking over ' + file);
    console.log(judged + ' judgement(s) and ' + skips + ' skip(s) across ' + analyses.length + ' set(s).');
    console.log('Skips are counted here and nowhere else -- they are not fed to the fit.');
    console.log('Prior: ' + PRIOR + ' win and ' + PRIOR + ' loss against a phantom average opponent, so an option that');
    console.log('lost everything has a finite strength. Intervals are 95%, from the inverse Fisher');
    console.log('information; on sparse data they are wide because the data IS thin, not as a formality.');

    for (const a of analyses) printSet(a);

    if (!judged) {
        console.log('');
        console.log('No fittable judgements anywhere in the log. Nothing to rank.');
    }
    console.log('');
}

if (process.argv[1] && pathToFileURL(process.argv[1]).href === import.meta.url) {
    main().catch((e) => { console.error(e && e.stack || e); process.exit(1); });
}
