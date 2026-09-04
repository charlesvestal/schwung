/**
 * page_nav.mjs — moving around a PageSet.
 *
 * PURE: takes a page list and an index, returns a new index. No input handling,
 * no state of its own; the caller owns the cursor. Both the native shadow UI
 * and a tool driving the same grid need identical navigation semantics, and the
 * only way to guarantee that is to put the rules in one place.
 *
 * Why this exists as more than `index + 1`:
 *
 *   - minijv plans to 69 grid pages and surge to 49. Linear jog through those
 *     is not navigation, it is a chore, so there is a level-skip step and a
 *     jump index.
 *   - 52 of the fleet's 572 grid pages are continuation pages holding one or
 *     two controls, an unavoidable consequence of a level with 9 keys and a
 *     first page that must stay exactly `knobs[0..7]` (see page_plan.mjs).
 *     They are cheap to skip rather than impossible to create.
 *   - the page set is rebuilt whenever the declared contract changes — a
 *     module finishing its load and republishing a bigger tree, a minijv mode
 *     switch. The cursor has to land somewhere sensible afterwards.
 */

import { PAGE_KNOBS } from "./page_plan.mjs";

/** Clamp an index into a page list. */
export function clampIndex(pages, index) {
    if (!pages || pages.length === 0) return 0;
    if (index < 0) return 0;
    if (index >= pages.length) return pages.length - 1;
    return index;
}

/**
 * Step one page. Does not wrap: on a 69-page module, wrapping from the last
 * page to the first turns an overshoot into a total loss of place.
 */
export function step(pages, index, delta) {
    const target = clampIndex(pages, clampIndex(pages, index) + delta);
    /*
     * HIDDEN pages are reachable but not walked.
     *
     * A page opened from a menu row belongs to that row, not to the sequence:
     * the CC Map's per-module pages would otherwise sit in the jog order after
     * it, so paging past "CC Map" walked into a module's list and out the other
     * side. goToPage still lands on them, which is how the row opens one.
     */
    if (!pages || !pages[target] || !pages[target].hidden) return target;
    const dir = delta < 0 ? -1 : 1;
    for (let i = target; i >= 0 && i < pages.length; i += dir) {
        if (!pages[i].hidden) return i;
    }
    /* Nothing visible that way: stay put rather than land on a hidden page. */
    return clampIndex(pages, index);
}

/**
 * Step to the next/previous *level* — the coarse gesture. Continuation pages
 * ("Filter - 2") belong to the level before them, so this lands on the first
 * page of the next distinct level, skipping the orphans.
 *
 * Falls back to a plain step when there is no further level in that direction,
 * so the gesture never feels dead.
 */
export function stepLevel(pages, index, delta) {
    if (!pages || pages.length === 0) return 0;
    const cur = clampIndex(pages, index);
    const from = pages[cur];
    const dir = delta < 0 ? -1 : 1;

    for (let i = cur + dir; i >= 0 && i < pages.length; i += dir) {
        if (pages[i].hidden) continue;
        if (!sameLevel(pages[i], from)) {
            /* Going backwards lands mid-level; rewind to that level's first page. */
            if (dir < 0) {
                let j = i;
                while (j - 1 >= 0 && sameLevel(pages[j - 1], pages[i])) j--;
                return j;
            }
            return i;
        }
    }
    return step(pages, cur, dir);
}

function sameLevel(a, b) {
    if (!a || !b) return false;
    /* `level` is null for the chain_params fallback pages; fall back to kind. */
    if (a.level === null && b.level === null) return a.kind === b.kind;
    return a.level === b.level;
}

/**
 * The jump index: one entry per level, not per page, because a picker listing
 * "Filter" and "Filter - 2" separately is the same chore in a different shape.
 * Each entry points at the level's first page.
 *
 * @returns {Array<{index:number, name:string, kind:string, pages:number}>}
 */
export function jumpIndex(pages) {
    const out = [];
    for (let i = 0; i < (pages || []).length; i++) {
        const p = pages[i];
        if (i > 0 && sameLevel(p, pages[i - 1]) && p.kind === pages[i - 1].kind) {
            out[out.length - 1].pages++;
            continue;
        }
        /* Continuation names carry a " - N" suffix; the index wants the level. */
        out.push({ index: i, name: String(p.name || "").replace(/ - \d+$/, ""), kind: p.kind, pages: 1 });
    }
    return out;
}

/**
 * The jump index folded one level further, by the parent prefix the planner
 * puts in front of a nested page name ("Tone1/Filter" → group "Tone1").
 *
 * minijv has 57 index entries but only ~15 groups, because four near-identical
 * tone subtrees account for most of them. Exposed as data rather than baked
 * into a picker so the caller can offer a flat list now and a two-level one
 * later without the library changing.
 *
 * @returns {Array<{name:string, index:number, entries:Array}>}
 */
export function groupIndex(pages) {
    const groups = [];
    /* The level's own page is named "Tone 1" while its children are prefixed
     * "Tone1/…" — the prefix drops the space. Comparing loosely merges the two
     * instead of listing "Tone 1" and "Tone1" as neighbouring sections, which
     * reads as a bug. The first-seen spelling is the one shown. */
    const norm = (s) => String(s).replace(/[\s_-]+/g, "").toLowerCase();
    /* Page names are unique, but the index strips their " - N" suffix, which can
     * bring a collision back: minijv declares a preset browser in BOTH its patch
     * and performance modes. Two sections called "Presets" is unpickable, so a
     * repeat gets numbered. */
    const used = new Map();
    for (const entry of jumpIndex(pages)) {
        const slash = entry.name.indexOf("/");
        /* "Common / Control" yields the group "Common " — trim it. */
        const group = (slash > 0 ? entry.name.slice(0, slash) : entry.name).trim();
        const last = groups[groups.length - 1];
        if (last && norm(last.name) === norm(last.baseName || last.name) && norm(last.baseName || last.name) === norm(group)) {
            last.entries.push(entry);
            last.pages += entry.pages;
            continue;
        }
        const seen = (used.get(norm(group)) || 0) + 1;
        used.set(norm(group), seen);
        groups.push({
            name: seen > 1 ? `${group} ${seen}` : group,
            baseName: group,
            index: entry.index, entries: [entry], pages: entry.pages,
        });
    }
    return groups;
}

/**
 * Where the cursor should land after the page set is rebuilt.
 *
 * Matching is by page NAME rather than position: a module that finishes loading
 * and republishes a larger tree (Virus, a minijv expansion) shifts every index,
 * and landing on "whatever is now at index 12" is worse than landing on the
 * page the user was actually looking at. Falls back to the same level, then to
 * the first grid page, then to 0.
 */
export function reanchor(oldPages, oldIndex, newPages) {
    if (!newPages || newPages.length === 0) return 0;
    const prev = (oldPages || [])[clampIndex(oldPages, oldIndex)];
    if (!prev) return firstGrid(newPages);

    const byName = newPages.findIndex((p) => p.name === prev.name && p.kind === prev.kind);
    if (byName >= 0) return byName;

    const byLevel = newPages.findIndex((p) => p.level && p.level === prev.level);
    if (byLevel >= 0) return byLevel;

    return firstGrid(newPages);
}

/** The first page worth showing: a grid if there is one, else page 0. */
export function firstGrid(pages) {
    if (!pages || pages.length === 0) return 0;
    const i = pages.findIndex((p) => p.kind === PAGE_KNOBS);
    return i >= 0 ? i : 0;
}
