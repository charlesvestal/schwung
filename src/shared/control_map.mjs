/*
 * control_map.mjs -- the per-set CONTROL DOCUMENT (set_state/<uuid>/controls.json).
 *
 *   { "version": 1,
 *     "surface": { "pages": [ { "name": "Drums", "knobs": [ target|null x16 ] } ] },
 *     "cc": [ ... ] }
 *
 * `surface` is the Custom layout's pages (layout_custom.mjs); `cc` is the
 * generic CC map's bindings (cc_map.mjs): { cable 2, channel 0-15, cc 0-127,
 * mode "abs" | "rel", target }, one binding per (channel, cc). Any OTHER key
 * is carried through a save VERBATIM, so an older build can never erase what
 * a newer one wrote.
 *
 * TOLERANT, NEVER THROWING. A file edited by hand, by the web editor, or by a
 * later build loads with whatever it cannot use left empty: a bad target is an
 * empty knob, a bad page is dropped, unparseable text is an empty document --
 * and the caller is told (`ok: false`) so it does not save over the file.
 *
 * Every edit returns a NEW document; nothing is mutated in place, so a caller
 * holding the old one (a screen mid-draw) is never looking at half an edit.
 */
import { normalizeTarget } from "./control_target.mjs";

export const CONTROLS_VERSION = 1;
export const MAX_PAGES = 16;
export const KNOBS_PER_PAGE = 16;
export const PAGE_NAME_MAX = 16;

const emptyKnobs = () => new Array(KNOBS_PER_PAGE).fill(null);
const cleanName = (n, i) => {
    const s = typeof n === "string" ? n.replace(/[\u0000-\u001f]/g, "").trim().slice(0, PAGE_NAME_MAX) : "";
    return s || "Page " + (i + 1);
};

export function emptyControls() {
    return { version: CONTROLS_VERSION, surface: { pages: [] }, cc: [], extra: {} };
}

export const CC_MODES = ["abs", "rel"];

/* A valid CC binding, or null. External input only (cable 2): Move's own
 * knobs are cable 0 and belong to each slot's Knob Mapping. */
export function normalizeBinding(b) {
    if (!b || typeof b !== "object") return null;
    const ch = b.channel, cc = b.cc;
    if (!Number.isInteger(ch) || ch < 0 || ch > 15 || !Number.isInteger(cc) || cc < 0 || cc > 127) return null;
    const target = normalizeTarget(b.target);
    if (!target) return null;
    return { cable: 2, channel: ch, cc, mode: CC_MODES.includes(b.mode) ? b.mode : "abs", target };
}

function normalizePage(p, i) {
    if (!p || typeof p !== "object") return null;
    const knobs = emptyKnobs();
    if (Array.isArray(p.knobs)) {
        for (let k = 0; k < KNOBS_PER_PAGE; k++) knobs[k] = normalizeTarget(p.knobs[k]);
    }
    return { name: cleanName(p.name, i), knobs };
}

/**
 * Text -> { doc, ok }. `ok` is false when the text was present but could not
 * be read as a control document; the caller must then NOT save, or a typo in
 * a hand-edited file would be answered by deleting everything in it.
 */
export function parseControls(text) {
    if (text === null || text === undefined || String(text).trim() === "") {
        return { doc: emptyControls(), ok: true };
    }
    let raw;
    try { raw = JSON.parse(String(text)); } catch (e) { return { doc: emptyControls(), ok: false }; }
    if (!raw || typeof raw !== "object" || Array.isArray(raw)) return { doc: emptyControls(), ok: false };
    const doc = emptyControls();
    const pages = raw.surface && Array.isArray(raw.surface.pages) ? raw.surface.pages : [];
    for (let i = 0; i < pages.length && doc.surface.pages.length < MAX_PAGES; i++) {
        const p = normalizePage(pages[i], doc.surface.pages.length);
        if (p) doc.surface.pages.push(p);
    }
    /* One binding per (channel, cc): a later one replaces an earlier. */
    if (Array.isArray(raw.cc)) {
        for (const b of raw.cc) {
            const n = normalizeBinding(b);
            if (!n) continue;
            doc.cc = doc.cc.filter((x) => !(x.channel === n.channel && x.cc === n.cc));
            doc.cc.push(n);
        }
    }
    for (const k of Object.keys(raw)) {
        if (k !== "version" && k !== "surface" && k !== "cc") doc.extra[k] = raw[k];
    }
    return { doc, ok: true };
}

export function serializeControls(doc) {
    const out = { version: CONTROLS_VERSION };
    out.surface = { pages: doc.surface.pages.map((p) => ({ name: p.name, knobs: p.knobs.slice() })) };
    out.cc = (doc.cc || []).map((b) => ({ cable: 2, channel: b.channel, cc: b.cc, mode: b.mode, target: b.target }));
    for (const k of Object.keys(doc.extra || {})) out[k] = doc.extra[k];
    return JSON.stringify(out, null, 1);
}

/* ---- edits: each returns a new document (or the same one when refused) ---- */

function withPages(doc, pages) {
    return { version: doc.version, surface: { pages }, cc: doc.cc || [], extra: doc.extra };
}

function withCC(doc, cc) {
    return { version: doc.version, surface: doc.surface, cc, extra: doc.extra };
}

/** Bind (channel, cc) to a target; replaces a binding on the same CC. */
export function bindCC(doc, binding) {
    const n = normalizeBinding(binding);
    if (!n) return doc;
    return withCC(doc, (doc.cc || []).filter((x) => !(x.channel === n.channel && x.cc === n.cc)).concat([n]));
}

export function unbindCC(doc, i) {
    const cc = doc.cc || [];
    if (!cc[i]) return doc;
    return withCC(doc, cc.filter((_, j) => j !== i));
}

export function setCCMode(doc, i, mode) {
    const cc = doc.cc || [];
    if (!cc[i] || !CC_MODES.includes(mode)) return doc;
    const next = cc.slice();
    next[i] = Object.assign({}, cc[i], { mode });
    return withCC(doc, next);
}

/** Insert a page at `at` (default: the end). Refused at MAX_PAGES. */
export function addPage(doc, at, name) {
    const pages = doc.surface.pages;
    if (pages.length >= MAX_PAGES) return doc;
    const i = at === undefined || at === null ? pages.length : Math.max(0, Math.min(pages.length, at | 0));
    const next = pages.slice();
    next.splice(i, 0, { name: cleanName(name, i), knobs: emptyKnobs() });
    return withPages(doc, next);
}

export function renamePage(doc, i, name) {
    const pages = doc.surface.pages;
    if (!pages[i]) return doc;
    const next = pages.slice();
    next[i] = { name: cleanName(name, i), knobs: pages[i].knobs };
    return withPages(doc, next);
}

export function deletePage(doc, i) {
    const pages = doc.surface.pages;
    if (!pages[i]) return doc;
    return withPages(doc, pages.filter((_, j) => j !== i));
}

export function movePage(doc, from, to) {
    const pages = doc.surface.pages;
    if (!pages[from] || to < 0 || to >= pages.length || from === to) return doc;
    const next = pages.slice();
    const [p] = next.splice(from, 1);
    next.splice(to, 0, p);
    return withPages(doc, next);
}

/** Put `target` on a knob (null clears it). An invalid target is refused. */
export function assignKnob(doc, page, knob, target) {
    const pages = doc.surface.pages;
    if (!pages[page] || knob < 0 || knob >= KNOBS_PER_PAGE) return doc;
    const t = target === null ? null : normalizeTarget(target);
    if (target !== null && !t) return doc;
    const knobs = pages[page].knobs.slice();
    knobs[knob] = t;
    const next = pages.slice();
    next[page] = { name: pages[page].name, knobs };
    return withPages(doc, next);
}

export const clearKnob = (doc, page, knob) => assignKnob(doc, page, knob, null);
