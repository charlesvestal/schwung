/*
 * control_editor.mjs -- Master FX Settings -> Surface Layout: the Custom
 * surface layout's pages, edited on Move. Pure; the host draws the rows and
 * routes jog / click / Back here.
 *
 *   PAGES   one row per page (its knob count beside it), then "Add Page"
 *   PAGE    Rename, Move Earlier, Move Later, Delete Page, then the sixteen
 *           knobs -- each naming what it drives. Clicking an assigned knob
 *           asks first ("Click: clear"), as Delete Page does: nothing on this
 *           screen destroys on a single click.
 *
 * Assigning is not done here: that is LEARN on the surface (hold a knob, move
 * a parameter on Move) or the web editor, which can list every parameter.
 * Every change goes through `edit(fn)` -- the host's load-modify-save.
 */
import { addPage, renamePage, deletePage, movePage, clearKnob, MAX_PAGES, KNOBS_PER_PAGE }
    from "./control_map.mjs";
import { KIND_PARAM, KIND_MASTER } from "./control_target.mjs";

/* Where a target lives, short: "S2", "MFX3", "Mst". */
export function scopeShort(t) {
    if (t.kind === KIND_PARAM) return "S" + (t.slot + 1);
    if (t.kind === KIND_MASTER) return "MFX" + t.fx;
    return t.slot === null ? "Mst" : "S" + (t.slot + 1);
}

export function createLayoutEditor(io) {
    const controls = io.controls;
    const edit = io.edit;
    let level = "pages";
    let page = 0;
    let cursor = 0;
    /* The row awaiting a second click, or null. Any move of the cursor drops it. */
    let confirm = null;

    const pages = () => controls().surface.pages;
    const PAGE_ACTIONS = ["rename", "up", "down", "delete"];

    function rows() {
        if (level === "pages") {
            const out = pages().map((p, i) => ({
                kind: "page", i, label: p.name,
                value: p.knobs.filter(Boolean).length + " knobs",
            }));
            if (pages().length < MAX_PAGES) out.push({ kind: "add", label: "Add Page", value: "" });
            return out;
        }
        const p = pages()[page];
        if (!p) return [];
        const out = [
            { kind: "rename", label: "Rename", value: p.name },
            { kind: "up", label: "Move Earlier", value: "" },
            { kind: "down", label: "Move Later", value: "" },
            { kind: "delete", label: "Delete Page", value: confirm === "delete" ? "Click: delete" : "" },
        ];
        for (let k = 0; k < KNOBS_PER_PAGE; k++) {
            const t = p.knobs[k];
            const key = "knob" + k;
            out.push({
                kind: "knob", k,
                label: "Knob " + (k + 1) + (t ? " " + scopeShort(t) : ""),
                value: confirm === key ? "Click: clear" : (t ? (t.label || t.key) : "--"),
            });
        }
        return out;
    }

    const clampCursor = () => { cursor = Math.max(0, Math.min(rows().length - 1, cursor)); };

    return {
        rows,
        get level() { return level; },
        get page() { return page; },
        get cursor() { return cursor; },
        title() { return level === "pages" ? "Surface Layout" : ((pages()[page] || {}).name || "Page"); },

        jog(delta) {
            confirm = null;
            cursor += delta;
            clampCursor();
            return rows()[cursor] || null;
        },

        /**
         * Click the row under the cursor. Returns what the host must do that
         * this module cannot: { rename: { page, name } } (open a keyboard), or
         * null. Everything else is done here, through edit().
         */
        click() {
            const r = rows()[cursor];
            if (!r) return null;
            if (level === "pages") {
                if (r.kind === "add") {
                    edit((doc) => addPage(doc));
                    cursor = pages().length - 1;
                    return null;
                }
                level = "page"; page = r.i; cursor = 0; confirm = null;
                return null;
            }
            if (r.kind === "rename") return { rename: { page, name: pages()[page].name } };
            if (r.kind === "up" && page > 0) { edit((doc) => movePage(doc, page, page - 1)); page--; return null; }
            if (r.kind === "down" && page < pages().length - 1) { edit((doc) => movePage(doc, page, page + 1)); page++; return null; }
            if (r.kind === "delete") {
                if (confirm !== "delete") { confirm = "delete"; return null; }
                edit((doc) => deletePage(doc, page));
                confirm = null; level = "pages"; cursor = Math.min(page, pages().length); page = 0;
                clampCursor();
                return null;
            }
            if (r.kind === "knob") {
                if (!pages()[page].knobs[r.k]) return null;
                const key = "knob" + r.k;
                if (confirm !== key) { confirm = key; return null; }
                edit((doc) => clearKnob(doc, page, r.k));
                confirm = null;
                return null;
            }
            return null;
        },

        /** The host's keyboard confirmed a new name. */
        rename(pageIndex, name) { edit((doc) => renamePage(doc, pageIndex, name)); },

        /** Back: a pending confirmation first, then up a level. True = leave. */
        back() {
            if (confirm) { confirm = null; return false; }
            if (level === "page") { level = "pages"; cursor = page; confirm = null; clampCursor(); return false; }
            return true;
        },

        /** Re-entering: start at the top; the document may have changed. */
        reset() { level = "pages"; page = 0; cursor = 0; confirm = null; },
    };
}
