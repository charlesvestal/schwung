/*
 * Module categories for the swap picker — "Sort: Type".
 *
 * WHERE THE CATEGORY COMES FROM. The catalog carries it (`subcategory` per
 * module, plus the display labels under the top-level `taxonomy` key), and
 * NOT the installed module.json — no fleet module declares one. So the
 * picker reads the copy schwung-manager keeps on disk, the same last-known-good
 * cache the manager itself renders from when GitHub is unreachable. A
 * module.json that DOES declare `subcategory` wins, which is how a built-in
 * (absent from the catalog) gets a heading at all.
 *
 * A module with no category is filed under "Other", LAST — never dropped. A
 * device whose manager has never fetched the catalog therefore shows one
 * "Other" group, i.e. the A-Z list with a heading, which is the honest answer.
 *
 * Pure: no globals, no host calls. The caller hands in a readFile.
 */

export const CATALOG_CACHE_PATH = "/data/UserData/schwung/manager-cache/catalog.json";
export const OTHER_LABEL = "Other";

/* The picker's component key → the taxonomy's component_type. */
export function taxonomyTypeFor(componentKey) {
    const k = String(componentKey || "");
    if (k === "synth") return "sound_generator";
    if (k === "midiFx" || k.indexOf("midi_fx") === 0) return "midi_fx";
    if (/^fx\d+$/.test(k)) return "audio_fx";
    return "";
}

/*
 * Read the cached catalog. Returns { byId, order } or null when there is no
 * usable file. `byId` maps module id → subcategory slug; `order` maps
 * component_type → [{ id, label }] in the taxonomy's own order, which is the
 * order the catalog site shows its chips in.
 */
export function loadCatalogCategories(readFile, path = CATALOG_CACHE_PATH) {
    let raw = null;
    try { raw = readFile(path); } catch (e) { raw = null; }
    if (!raw) return null;
    let cat;
    try { cat = JSON.parse(raw); } catch (e) { return null; }
    if (!cat || typeof cat !== "object") return null;

    const byId = Object.create(null);
    for (const m of (Array.isArray(cat.modules) ? cat.modules : [])) {
        if (m && m.id && m.subcategory) byId[m.id] = String(m.subcategory);
    }
    const order = Object.create(null);
    const subs = cat.taxonomy && cat.taxonomy.subcategories;
    if (subs && typeof subs === "object") {
        for (const type of Object.keys(subs)) {
            order[type] = (Array.isArray(subs[type]) ? subs[type] : [])
                .filter(s => s && s.id)
                .map(s => ({ id: String(s.id), label: String(s.label || s.id) }));
        }
    }
    return { byId, order };
}

/* A slug with no label anywhere still reads as words: "mono-bass" → "Mono Bass". */
function humanise(slug) {
    return String(slug).split(/[-_]+/).filter(Boolean)
        .map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(" ");
}

/*
 * Group module rows by category, with a divider row heading each group.
 *
 * `rows` are picker module rows ({ id, name, subcategory? }), already sorted
 * A-Z. Only these are grouped — the caller keeps the synthetic rows (None,
 * the filter/sort rows, Move Left/Right, [Get more...]) where they belong.
 *
 * Group order: the taxonomy's order for this component type, then any slug
 * the taxonomy does not know (alphabetically), then Other. Within a group the
 * incoming order is kept, so A-Z stays A-Z.
 */
export function groupRowsByCategory(rows, componentType, catalog) {
    const byId = (catalog && catalog.byId) || {};
    const known = (catalog && catalog.order && catalog.order[componentType]) || [];
    const labelOf = Object.create(null);
    for (const s of known) labelOf[s.id] = s.label;

    const groups = new Map();          /* slug ("" = Other) → rows */
    for (const r of rows || []) {
        if (!r) continue;
        const slug = r.subcategory || byId[r.id] || byId[r.parentId] || "";
        if (!groups.has(slug)) groups.set(slug, []);
        groups.get(slug).push(r);
    }

    const ordered = [];
    for (const s of known) if (groups.has(s.id)) ordered.push(s.id);
    const unknown = [...groups.keys()]
        .filter(k => k && labelOf[k] === undefined)
        .sort((a, b) => humanise(a).localeCompare(humanise(b)));
    ordered.push(...unknown);
    if (groups.has("")) ordered.push("");

    const out = [];
    for (const slug of ordered) {
        const label = slug ? (labelOf[slug] || humanise(slug)) : OTHER_LABEL;
        out.push({ type: "divider", label, id: "__cat__" + slug });
        out.push(...groups.get(slug));
    }
    return out;
}

export function isCategoryHeader(row) {
    return !!(row && row.type === "divider");
}
