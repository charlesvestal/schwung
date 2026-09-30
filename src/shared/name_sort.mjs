/*
 * name_sort.mjs -- the one order every name list on the device uses.
 *
 * QuickJS's String.prototype.localeCompare compares CODE POINTS, so every
 * `a.name.localeCompare(b.name)` sorted every capital before every lowercase
 * letter: "dAVEBOx" landed after "Wave Edit" at the bottom of the Tools list,
 * and "Set 10" before "Set 9". People read these lists alphabetically.
 *
 * compareNames: case-insensitive, with runs of digits compared as NUMBERS
 * ("Set 9" < "Set 10"). Ties -- names equal ignoring case -- fall back to the
 * code-point order, so the sort is total and deterministic.
 */

const CHUNK = /(\d+)/;

export function compareNames(a, b) {
    const x = String(a == null ? "" : a);
    const y = String(b == null ? "" : b);
    const xs = x.toLowerCase().split(CHUNK);
    const ys = y.toLowerCase().split(CHUNK);
    const n = Math.min(xs.length, ys.length);
    for (let i = 0; i < n; i++) {
        const p = xs[i], q = ys[i];
        if (p === q) continue;
        if (i % 2 === 1) {                        /* both are digit runs */
            const d = Number(p) - Number(q);
            if (d !== 0) return d < 0 ? -1 : 1;
            if (p.length !== q.length) return p.length < q.length ? -1 : 1;  /* "02" after "2" */
            continue;
        }
        return p < q ? -1 : 1;
    }
    if (xs.length !== ys.length) return xs.length < ys.length ? -1 : 1;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* For .sort() over objects: byName("name"), byName("title"), byName("label"). */
export function byName(field) {
    return (a, b) => compareNames(a && a[field], b && b[field]);
}
