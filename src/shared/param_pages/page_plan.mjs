/**
 * page_plan.mjs — turn a module's declared UI contract into an ordered list of
 * knob pages.
 *
 * PURE. No param I/O, no drawing, no globals, no module-level state. Inputs are
 * plain objects; the caller does every read and write. This is what lets the
 * same planner serve the native shadow UI (which owns input and commits values)
 * and a tool module like a sequencer (which owns input and commits to its own
 * step data for parameter locks). See
 * docs/plans/2026-07-26-param-pages-audit.md.
 *
 * The tree walk follows the rules validated against a 76-module fleet capture:
 * visit both `params` nav edges and `children` (always, not either/or), no
 * depth cap, dedupe authored pages by exact key list, `children`-reached levels
 * are prefix-transparent, orphan sweep at the end so every declared knob is
 * reachable.
 *
 * Portions derived from schwung-movy's hierarchy walk
 * (c) 2026 megadake, MIT — https://github.com/DimaDake/schwung-movy
 */

import { hasChildren, childCount, childIndexParam, childName } from "./child_key.mjs";

/**
 * Every param key the hierarchy lists ANYWHERE — so the planner can ask
 * whether a key is reachable by the user at all.
 *
 * Nav entries (`{level: …}`) are not params and are skipped, exactly as
 * buildMetaIndex skips them.
 */
function allListedKeys(hierarchy) {
    const out = new Set();
    const add = (k) => { if (typeof k === "string" && k) out.add(k); };
    for (const lvl of Object.values((hierarchy && hierarchy.levels) || {})) {
        if (!lvl || typeof lvl !== "object") continue;
        /*
         * LISTED IS NOT REACHABLE, and the difference is a trap.
         *
         * An overflow key pulled from `params[]` is dropped when it is
         * `ui_`-prefixed -- see the filter that builds `extra` below, whose
         * comment names ui_current_pad and ui_preset_path outright. So a
         * module can list its index param in params[] and that param still
         * never gets a cell.
         *
         * This has to agree with that filter exactly. It did not, and the cost
         * was the picker being suppressed as "redundant" while the control it
         * deferred to did not exist: with auto-select off there was then NO
         * way to change instance at all. Reported from the device as "if I
         * turn off autoselect how do I get to another pad's settings?".
         *
         * A key on `knobs[]` is the author's intent and is honoured whatever
         * it is called, which is why the two lists are treated differently
         * here rather than merged.
         */
        for (const p of (lvl.params || [])) {
            const k = (p && typeof p === "object") ? (p.level ? null : p.key) : p;
            if (typeof k === "string" && k && !/^ui_/.test(k)) add(k);
        }
        for (const k of (lvl.knobs || [])) {
            if (k && typeof k === "object") add(k.key); else add(k);
        }
    }
    return out;
}

/**
 * Does this child level still need a picker PAGE?
 *
 * The picker exists because nothing else owns the focus. Once a level declares
 * `child_index_param` the MODULE owns it — and if that param is also listed
 * somewhere the user can reach, the picker becomes a second control for a fact
 * that already has one. This tree treats that as a defect rather than a
 * convenience: the list arrows went when the scrollbar arrived, and the file
 * browser's `13/30` counter went for the same reason.
 *
 * It showed up the moment a module declared TWO child levels sharing one
 * index: mrdrums got a pad picker on root AND on Pad Settings, both selecting
 * the same pad, in sync with each other and with the pad you played. Reported
 * from the device as exactly that question.
 *
 * REACHABILITY IS THE CONDITION, not merely the declaration. A module that
 * names an index param it never lists would otherwise lose its picker and
 * leave no way at all to change instance by hand — so in that case the picker
 * stays, and a module gets the deduplication only by offering the control
 * itself.
 */
function childPickerNeeded(lvl, listedKeys, pickedFor) {
    const idxParam = childIndexParam(lvl);
    if (!idxParam) return true;
    /*
     * ONE PICKER PER SHARED INDEX.
     *
     * Two levels naming the same `child_index_param` are two views of one
     * focus -- mrdrums has root and Pad Settings both following
     * ui_current_pad -- so a picker on each is two lists selecting the same
     * pad, in sync with each other and with the pad you played. Reported from
     * the device as "why is main pad selection AND pad settings pad
     * selection?".
     *
     * The FIRST level to plan one keeps it; the rest defer. Walk order is
     * stable, so which level that is does not wander between plans.
     */
    if (pickedFor.has(idxParam)) return false;
    /* ...and no picker at all when the module offers a real cell for it. */
    return !listedKeys.has(idxParam);
}

import { alignGroupsToRows, gatherGroupMembers } from "./viz.mjs";
import { buildMetaIndex } from "./param_meta.mjs";
export const KNOBS_PER_PAGE = 8;

/* Page kinds. Only PAGE_KNOBS needs a new renderer; every other kind dispatches
 * to a screen the shadow UI already draws. */
export const PAGE_KNOBS  = "knobs";   /* grid of up to 8 turnable params */
export const PAGE_PRESET = "preset";  /* list_param/count_param/name_param browser */
export const PAGE_ITEMS  = "items";   /* items_param/select_param runtime list */
/*
 * A list of entries that are NOT parameters — an action with a name and a
 * consequence and nothing to show, or a plain jump to another level.
 *
 * The other four non-grid kinds are all param-driven (a preset browser needs
 * list_param/count_param, an items list needs items_param), so none of them can
 * express "Save / Save As / Delete / Knob Mapping". Rendering those as knob
 * cells spends the whole 15-row widget band on six words, because there is no
 * value to draw. A level declares one with:
 *
 *   menu: [ { label: "Save", action: "save" },
 *           { label: "LFO 1", level: "lfo1" },
 *           { label: "Mode", action: "mode", value: "Poly" } ]
 *
 * `value` is optional and right-aligned, so a settings menu reads like the list
 * it replaces. This kind is drawn by the LIBRARY (renderPicker with
 * header:false) rather than handed to a host screen — it is just a list, and a
 * tool embedding the grid should get menus without owning a screen.
 */
export const PAGE_MENU   = "menu";

/* Param types that cannot be driven by turning a knob. They keep their grid
 * position (the module put them there) but open a fullscreen editor on click,
 * exactly as the list editor does today. Not a page kind — a cell behaviour. */
export const OPAQUE_TYPES = new Set(["canvas", "wav_position", "string", "filepath", "file"]);

/* ---------------------------------------------------------------- helpers */

function keyOf(entry) {
    if (typeof entry === "string") return entry;
    if (entry && typeof entry === "object" && entry.key) return entry.key;
    return null;
}

/* A nav entry points at another level and carries no param of its own. */
function levelOf(entry) {
    return (entry && typeof entry === "object" && entry.level) ? entry.level : null;
}

/**
 * A level's display name usually lives on the nav ENTRY that points at it, not
 * on the level itself, so this collects labels from every level's nav entries.
 * Nav label beats the level's own `label`: 24 levels across
 * dexed/linein/minijv/obxd/sf2/sfz/nam disagree, and the nav label is the one
 * users already see.
 *
 * Exported with `declaredLevelName` below because "what is this level called"
 * is ONE fact with more than one consumer — the page title and the voice list
 * — and a fact with several consumers written down in none of them is how the
 * metronome and recall_quantize both got the same off-by-one. voices.mjs
 * re-spelled two of the three sources as `level.name || levelKey` and the two
 * measurably disagreed: a nav link `{level: "bd", label: "Bass Drum"}` gave the
 * page header "Bass Drum" and the picker/voice list "bd", for the same thing.
 */
export function navLabelsOf(levels) {
    const navLabel = Object.create(null);
    for (const lvl of Object.values(levels || {})) {
        for (const p of ((lvl && lvl.params) || [])) {
            const target = levelOf(p);
            if (target && p.label) navLabel[target] = p.label;
        }
    }
    return navLabel;
}

/**
 * The name a level DECLARES, from its three sources in priority order, or null
 * when it declares none. Null is deliberate and load-bearing: the caller picks
 * the fallback, and they differ — a page title prettifies the key ("osc1" ->
 * "Osc1") while a voice name keeps the key verbatim, because a voice name is an
 * identity a sequencer matches on, not chrome. Folding a fallback in here would
 * force one of those on the other.
 *
 * `navLabel` is what navLabelsOf() returned for the same `levels` object;
 * passing it in keeps this a pure function of its arguments and keeps the O(n)
 * scan out of a per-level call.
 */
export function declaredLevelName(key, lvl, navLabel) {
    return (lvl && lvl.name) || (navLabel && navLabel[key]) || (lvl && lvl.label) || null;
}

/* `children` is absent as null, missing, or the literal string "None" — dexed
 * and every minijv level serialise it that way. */
function childOf(level) {
    const c = level && level.children;
    return (c && c !== "None") ? c : null;
}

/**
 * Which of `modes` a reported mode value names, or the first mode as the
 * fallback. Exported because the seeding path in the shadow UI has to agree
 * with the planner about this: the module reports "Performance" and the level
 * is called "performance", so anything comparing them exactly reads a module
 * in its second mode as being in its first.
 */
export function pickMode(reported, modes) {
    if (!Array.isArray(modes) || modes.length === 0) return null;
    if (reported === null || reported === undefined) return modes[0];
    const want = String(reported).trim().toLowerCase();
    if (!want) return modes[0];
    const named = modes.find((m) => String(m).trim().toLowerCase() === want);
    if (named !== undefined) return named;
    /* A module may answer with the enum INDEX instead of the name. Name first,
     * deliberately, so a mode literally called "1" still resolves as itself. */
    const n = parseInt(want, 10);
    if (Number.isInteger(n) && n >= 0 && n < modes.length) return modes[n];
    return modes[0];
}

/* Every level reachable from `root` by the same two edges the walk follows —
 * a `params` entry naming a level, and `children`. Used to work out which
 * levels belong to a mode OTHER than the active one; the walk itself stays
 * recursive-with-prefixes and cannot answer that question on its own. */
function reachableFrom(levels, root) {
    const seen = new Set();
    const stack = [root];
    while (stack.length) {
        const key = stack.pop();
        if (seen.has(key)) continue;
        const lvl = levels[key];
        if (!lvl) continue;
        seen.add(key);
        for (const p of (lvl.params || [])) {
            const t = levelOf(p);
            if (t) stack.push(t);
        }
        const kid = childOf(lvl);
        if (kid) stack.push(kid);
    }
    return seen;
}

function knobKeys(level) {
    return ((level && level.knobs) || []).map(keyOf).filter((k) => k !== null);
}

/* Editable (non-nav) keys a level lists in `params`, in declaration order. */
function paramKeys(level) {
    return ((level && level.params) || [])
        .filter((p) => !levelOf(p))
        .map(keyOf)
        .filter((k) => k !== null);
}

/* 6-char parent tag: one word → first 6 chars, multi-word → first 4 of word one
 * plus the other words' initials ("Operator 1" → "Oper1"). The 128 px header
 * also carries the module name, so this has to stay short. */
function levelNameToPrefix(name) {
    const words = String(name || "").split(/\s+/).filter(Boolean);
    if (words.length === 0) return "";
    if (words.length === 1) return words[0].slice(0, 6);
    return (words[0].slice(0, 4) + words.slice(1).map((w) => w[0].toUpperCase()).join("")).slice(0, 6);
}

/* FNV-1a over the declared contract. The page set is rebuilt when this changes:
 * module swap, an is_loading→ready re-fetch that rewrites the tree (Virus,
 * minijv expansions), or a mode change. */
function fingerprintOf(parts) {
    let h = 0x811c9dc5;
    const s = JSON.stringify(parts);
    for (let i = 0; i < s.length; i++) {
        h ^= s.charCodeAt(i);
        h = (h + ((h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24))) >>> 0;
    }
    return h.toString(16);
}

function chunk(arr, size) {
    const out = [];
    for (let i = 0; i < arr.length; i += size) out.push(arr.slice(i, i + size));
    return out;
}

/* Shared by a level's own `menu` and the caller-supplied `trailingMenus`: both
 * are lists of {label, action?, level?, value?} and both render through the
 * same PAGE_MENU. A raw entry with no `label` is dropped — a menu row with no
 * text is not a control, it is a hole — which is why the CALLER must check the
 * length of what this returns, not the length of what it was given. */
function mapMenuEntries(rawEntries) {
    return (Array.isArray(rawEntries) ? rawEntries : []).map((e) => ({
        label: String((e && e.label) || ""),
        action: (e && e.action) || null,
        target: (e && e.level) || null,
        value: (e && e.value) !== undefined ? e.value : null,
    })).filter((e) => e.label);
}

/*
 * A page name is the only handle the user has on the module, and it is what
 * reanchor() matches on after a rebuild, so uniqueness is not cosmetic (see
 * claimName's fuller note at its call site). Two independent claimers are
 * needed rather than one: the fallback (`chain_params`, no `levels`) return
 * happens BEFORE `claimName` exists lexically, so it needs its own `Set` —
 * but that only justifies two Sets, not two copies of the numbering loop.
 */
export function makeClaimer(used) {
    return (base) => {
        if (!used.has(base)) { used.add(base); return base; }
        for (let n = 2; ; n++) {
            const candidate = `${base} - ${n}`;
            if (!used.has(candidate)) { used.add(candidate); return candidate; }
        }
    };
}

/*
 * The caller-supplied trailingMenus, turned into PAGE_MENU page objects.
 *
 * Extracted out of appendTrailing (below) so page_controller.mjs's
 * refreshTrailing can rebuild ONLY the trailing pages — after a Save/Delete
 * changes what a "My Presets" row offers — without a second copy of the
 * entry transform or the name-collision loop. `claim` is a naming function
 * from makeClaimer, already bound to whatever names must not collide.
 */
export function buildTrailingPages(trailingMenus, claim) {
    const built = [];
    const warnings = [];
    for (const m of (trailingMenus || [])) {
        if (!m) continue;
        /*
         * The guard must run on the MAPPED result, not the raw one: an
         * entry whose only items lack a `label` (a typo'd or missing
         * field) still has a nonzero raw length, so a length check on
         * `m.entries` before mapping lets it through and produces a
         * real, empty, inert PAGE_MENU page — silently, since nothing
         * downstream flags an empty entry list as wrong.
         */
        const entries = mapMenuEntries(m.entries);
        if (entries.length === 0) {
            warnings.push(`trailingMenus "${m.name || "Menu"}" produced no usable entries — skipped`);
            continue;
        }
        built.push({
            kind: PAGE_MENU,
            name: claim(String(m.name || "Menu")),
            level: null,
            trailing: true,
            entries,
        });
    }
    return { pages: built, warnings };
}

/**
 * Split into the same number of pages a greedy fill would use, but spread
 * evenly — 9 keys become 5+4 rather than 8+1.
 *
 * Only ever applied to OVERFLOW keys. The authored page must stay exactly
 * `knobs[0..7]`, because the shim maps the physical knobs to that same array
 * (buildKnobContext in shadow_ui.js); if the grid's first page disagreed, one
 * knob would do different things in the list and on the grid.
 */
function balancedChunk(arr, size) {
    if (arr.length <= size) return arr.length ? [arr] : [];
    const pages = Math.ceil(arr.length / size);
    const per = Math.ceil(arr.length / pages);
    return chunk(arr, per);
}

/* ------------------------------------------------------------------ plan */

/**
 * @param {object}   o
 * @param {object}   o.hierarchy    parsed ui_hierarchy (null = the module
 *                                  declares none)
 * @param {Array}    o.chainParams  parsed chain_params (may be null/empty)
 * @param {boolean}  [o.unresolved] the contract could not be READ. Plans
 *                                  nothing at all — see the note in the body.
 * @param {string}   [o.mode]       active value of hierarchy.mode_param
 * @param {Function} [o.visible]    (condition, levelDef) => boolean. Lets the
 *                                  caller wire the shadow UI's visible_if
 *                                  evaluator without the planner reading params.
 *                                  Default: everything visible (fail-open, which
 *                                  is what the native evaluator does too).
 * @param {Array}    [o.trailingMenus] Caller-supplied PAGE_MENU pages appended
 *                                  AFTER the whole walk — e.g. "My Presets"
 *                                  and "Module". Opt-in: absent for a tool
 *                                  embedding this grid with no slot to swap a
 *                                  module in. See the note above appendTrailing.
 * @returns {{pages: Array, fingerprint: string, warnings: string[]}}
 */
/**
 * A level's own cell labels: { key: short_name } from its inline params/knobs.
 *
 * Returns null when the level declares none, so a page that wants nothing
 * carries nothing and every existing consumer is unaffected.
 */
export function levelShortNames(lvl) {
    if (!lvl) return null;
    let out = null;
    const take = (e) => {
        if (!e || typeof e !== "object" || !e.key) return;
        if (typeof e.short_name !== "string" || !e.short_name) return;
        (out = out || {})[e.key] = e.short_name;
    };
    for (const e of (lvl.params || [])) take(e);
    for (const e of (lvl.knobs || [])) take(e);
    return out;
}

export function planPages({ hierarchy, chainParams, mode, visible, unresolved,
                            trailingMenus, paginate = true } = {}) {
    /*
     * `paginate: false` means "this level is ONE page, however long it is".
     *
     * Eight is the number of physical knobs, and chunking a level at eight is
     * the GRID's constraint — it has eight cells and there is nowhere to put a
     * ninth. A list has no such limit: it draws five rows of a page and scrolls
     * the rest, and `knobRows()` reads the page's keys with no cap, so a page
     * of any length lists correctly today.
     *
     * Global Settings is pinned to the list (`layout: LAYOUT_LIST`, see
     * paramPagesLayout) and was still being planned as a grid, so a ninth
     * param in a section silently became a second page named "<Section> - 2"
     * holding one row — a jog step nobody chose, on a screen where the list
     * was already scrolling. That is the grid's rule leaking into a screen the
     * grid never draws.
     *
     * A property of the CONTRACT, exactly like the layout pin it accompanies,
     * and NOT derived from the layout: the layout is also LAYOUT_LIST when the
     * screen reader is on or Param View is set to List, and un-paginating every
     * module in those cases would rearrange 95 modules' pages behind a
     * preference. A module's pages are authored groupings; these are sections.
     *
     * Default true — every existing caller keeps the grid's chunking.
     */
    const perPage = paginate ? KNOBS_PER_PAGE : Infinity;
    const warnings = [];
    /* Whether a child level still needs a picker PAGE depends on the WHOLE
     * hierarchy, not on the level: the index param may be listed on a sibling
     * level, which is exactly where mrdrums puts it. */
    const listedKeys = allListedKeys(hierarchy);
    /* child_index_param -> a picker page already exists for it. See
     * childPickerNeeded: two levels sharing one focus need ONE list. */
    const pickedFor = new Set();
    /*
     * Built once per plan so knob pages can be nudged into a drawable layout —
     * see alignGroupsToRows. Planning happens on component load, not per frame,
     * and the alignment pass is pure array work on at most 8 keys.
     */
    let _metaIndex = null;
    const metaIndexOnce = () => {
        if (!_metaIndex) _metaIndex = buildMetaIndex({ hierarchy, chainParams });
        return _metaIndex;
    };
    /* Records every page whose knobs we reordered, so validate_contract can
     * tell an author their layout was adjusted rather than leaving them to
     * wonder why a knob moved. */
    const realigned = [];
    const alignKnobs = (ks, where) => {
        /* GATHER FIRST, THEN ALIGN. Gathering makes a scattered group
         * contiguous; aligning moves an already-contiguous group off the row
         * break. Running align first would find nothing to rescue, because a
         * group whose members are three knobs apart is not straddling — it is
         * not a group yet. */
        const gathered = gatherGroupMembers(ks, metaIndexOnce());
        if (gathered.moved) {
            realigned.push({ page: where, from: -1, to: -1, span: gathered.span });
        }
        const r = alignGroupsToRows(gathered.keys, metaIndexOnce());
        if (r.moved) realigned.push({ page: where, from: r.from, to: r.to, span: r.span });
        return r.keys;
    };
    /* Keys any visible_if condition reads. Returned so the caller can re-plan
     * when one of THEM changes — a condition is driven by a param VALUE, which
     * moves without the declared contract moving, so the fingerprint cannot see
     * it. Without this a level's params never appear or disappear in response to
     * the switch that gates them. */
    const conditionKeys = new Set();
    const noteCondition = (cond) => {
        if (!cond || typeof cond !== "object") return;
        const k = cond.param || cond.key || cond.param_key;
        if (k) conditionKeys.add(String(k));
    };
    const isVisible = (cond, lvl) => {
        noteCondition(cond);
        return cond ? (visible ? !!visible(cond, lvl) : true) : true;
    };
    /* The fingerprint covers chain_params CONTENT, not just its length. A
     * module that finishes loading and republishes real enum options in place
     * (osirus swaps rom_index from ["(loading)"] to the Virus models) changes no
     * count and no level — hashing the length alone would call that unchanged
     * and leave the placeholder on screen for the rest of the session. */
    const fingerprint = fingerprintOf([hierarchy || null, chainParams || null, mode || null]);

    /*
     * "the module declares no hierarchy" and "we could not READ the hierarchy"
     * are different answers, and collapsing them is a bug with a visible
     * symptom. On the wire:
     *
     *   JSON   the module declares this hierarchy
     *   ""     the module declares NO hierarchy       -> the fallback below
     *   null   the read FAILED, we know NOTHING       -> `unresolved: true`
     *
     * Both of the first two parse to a value the planner can act on, and a
     * failed read parses to nothing — which is indistinguishable from absence
     * by the time it reaches here. So the caller, who is the only one that saw
     * the wire, says so explicitly. (`hierarchy: null` on its own still means
     * ABSENT: that is what the fleet capture records for the four modules that
     * publish chain_params and nothing else.)
     *
     * Picking a sample in granny loads the WAV synchronously on the thread that
     * serves param requests; the ui_hierarchy read issued milliseconds later
     * times out, and paginating chain_params in response put `sample_path` —
     * granny's first declared param — on knob 1 and shifted every other knob
     * along, until a full teardown of the controller.
     *
     * A plan is a statement about what a module declares. With a failed read we
     * have no such statement, so we make none.
     */
    if (unresolved) {
        /* Left alone, deliberately: trailingMenus is never appended here.
         * A failed read must never manufacture a "Remove Module" button — a
         * plan is a statement about what a module declares, and a failed read
         * has no statement to make. */
        return {
            pages: [], fingerprint, conditionKeys: new Set(), unresolved: true,
            warnings: ["ui_hierarchy unresolved — the contract read failed"],
        };
    }

    /*
     * Pages appended after the WHOLE walk, supplied by the caller.
     *
     * Not a level's `menu`: a level emits its menu straight after its own
     * grids and before any level it navigates to, so a menu on root lands
     * second. Slot Settings gives its menu its own level to dodge that
     * (shadow_ui_slot_grid.mjs), which works only because it synthesises its
     * whole hierarchy. We do not own a module's, and three fleet shapes make
     * injection impossible anyway: 11 of 95 modules publish no `levels` object,
     * minijv has no `root`, and with `modes` the walk root is the active mode.
     *
     * Opt-in, because a tool embedding this grid for parameter locks has no
     * slot to swap a module in. The planner never learns what an action MEANS
     * — that stays with the host, the same boundary that keeps actions out of
     * the editors.
     */
    const appendTrailing = (pages, claim) => {
        const built = buildTrailingPages(trailingMenus, claim);
        for (const w of built.warnings) warnings.push(w);
        for (const p of built.pages) pages.push(p);
        return pages;
    };

    const levels = (hierarchy && hierarchy.levels && typeof hierarchy.levels === "object")
        ? hierarchy.levels : null;

    /* No hierarchy at all — 4 modules in the fleet (branchage, belt-in,
     * po32-drum, smack-in) publish chain_params and nothing else. Paginate the
     * declared params so they are not a blank screen.
     *
     * The "no hierarchy AND no chain_params" branch below is left alone,
     * deliberately: it already produces nothing, and a view made only of
     * trailing pages is a shape no consumer expects. The Shift+Click route
     * still reaches Swap/Remove there. */
    if (!levels) {
        const keys = (chainParams || []).map((p) => p && p.key).filter(Boolean);
        if (keys.length === 0) return { pages: [], fingerprint, warnings: ["no ui_hierarchy and no chain_params"], conditionKeys: new Set() };
        warnings.push("no ui_hierarchy — paginated from chain_params");
        const pages = chunk(keys, perPage).map((ks, i) => {
            const name = i === 0 ? "Params" : `Params - ${i + 1}`;
            return { kind: PAGE_KNOBS, name, level: null,
                     keys: alignKnobs(ks, name), authored: false };
        });
        /* No `claimName` in scope here — this branch never names by level, so
         * give the append its own local claim over the names already used
         * (see makeClaimer's note: two Sets, not two copies of the loop). */
        const fallbackClaim = makeClaimer(new Set(pages.map((p) => p.name)));
        return { pages: appendTrailing(pages, fallbackClaim), fingerprint, warnings, realigned };
    }

    for (const lvl of Object.values(levels)) {
        if (!lvl || typeof lvl !== "object") continue;
        noteCondition(lvl.visible_if);
        for (const p of (lvl.params || [])) {
            if (p && typeof p === "object") noteCondition(p.visible_if);
        }
    }

    const pages = [];

    const navLabel = navLabelsOf(levels);
    /* A level key is an internal identifier ("root", "patch_main", "osc1"); it
     * is only a last resort for a page title, and never raw — "osc1" reads as
     * "Osc1", not as a variable name. */
    const prettify = (key) => String(key)
        .replace(/[_-]+/g, " ")
        .replace(/\b[a-z]/g, (c) => c.toUpperCase());
    const declaredName = (key, lvl) => declaredLevelName(key, lvl, navLabel);
    const nameOf = (key, lvl) => declaredName(key, lvl) || prettify(key);

    /**
     * Allocate a page name, appending " - N" for the smallest free N when the
     * base is taken. EVERY page kind goes through this, not just grids: minijv
     * declares a preset browser in both its patch and performance modes, and two
     * sections both called "Presets" is unusable in the picker — and worse,
     * reanchor() matches by name after a rebuild and would land on the wrong one.
     *
     * It serves two jobs with one mechanism: numbering a level's continuation
     * pages, and disambiguating genuine collisions —
     * freak and granny each declare a child level called "main" alongside
     * root's own "Main" page, with different knobs, so both must appear and
     * neither may be silently renamed away.
     *
     * A page name is the only handle the user has on a 76-page module, and it
     * is what reanchor() matches on after a rebuild, so uniqueness is not
     * cosmetic.
     */
    const usedNames = new Set();
    const claimName = makeClaimer(usedNames);

    /* Mode select (minijv only in the fleet): with `modes` present the level
     * names ARE the mode names, so the active mode picks the walk root. */
    let rootKey = "root";
    const inactiveModeLevels = new Set();
    const modes = Array.isArray(hierarchy.modes) ? hierarchy.modes : null;
    if (modes && modes.length > 0) {
        /*
         * The mode selector IS an items page too -- a list, a cursor, a
         * current marker, click to choose -- so it reuses that machinery
         * instead of being a kind nothing draws. PAGE_MODES was planned and
         * rendered by NOBODY, so minijv's Mode page ejected to the list
         * editor, which is also why its performance mode was unreachable from
         * the grid (and therefore why editing parts appeared to do nothing).
         *
         * Unlike a child selector this one DOES write a param: the module
         * takes the mode name. Unlike an ordinary items page the choice
         * re-roots the whole hierarchy, so it also forces a re-plan.
         */
        /*
         * The rows are the LEVEL names, which are lowercase because levels are
         * — minijv drew "patch" / "performance" against a fleet where every
         * other list is title-case. The module already spells them properly in
         * chain_params (`options: ["Patch", "Performance"]`), so borrow that.
         *
         * Only where the option is the same WORD, case aside. Anything else is
         * a relabelling, and `derivedLabels` is not merely a label: commitItem
         * takes the chosen entry as the mode value. Restricting it to
         * capitalisation is what makes this cosmetic and keeps the two in step.
         */
        const modeOpts = (chainParams || []).find(
            (p) => p && p.key === (hierarchy.mode_param || "mode"));
        const optionFor = (name) => (modeOpts && Array.isArray(modeOpts.options))
            ? modeOpts.options.find((o) => String(o).toLowerCase() === String(name).toLowerCase())
            : undefined;
        pages.push({
            kind: PAGE_ITEMS, name: claimName("Mode"), level: null,
            derivedLabels: modes.map((mo) => optionFor(mo) !== undefined ? optionFor(mo) : mo),
            selectParam: hierarchy.mode_param || "mode",
            modeSelect: true,
        });
        /* Case-insensitive on purpose. The level names, and therefore the
         * `modes` array, are lowercase; minijv's get_param("mode") answers
         * "Performance". An exact match silently fell through to modes[0], so
         * a module sitting in its second mode planned its first one. */
        const active = pickMode(mode, modes);
        rootKey = levels[active] ? active : "root";
        if (!levels[active]) warnings.push(`mode "${active}" has no matching level`);
        /*
         * Levels owned by a mode that is NOT the active one.
         *
         * The orphan sweep at the bottom of the walk cannot tell "no edge
         * reaches this" from "the other mode reaches this" — and it was
         * written FOR minijv's performance tree, back when the Mode page was
         * unreachable and those levels genuinely were orphans. Left alone it
         * undoes the re-root one loop later: measured, both modes planned all
         * 76 pages, differing only in which level got named "Main".
         *
         * Reachable-from-active wins, so a level both trees share is kept.
         */
        for (const other of modes) {
            if (other === active) continue;
            for (const k of reachableFrom(levels, other)) inactiveModeLevels.add(k);
        }
        for (const k of reachableFrom(levels, active)) inactiveModeLevels.delete(k);
    }
    if (!levels[rootKey]) {
        const first = Object.keys(levels)[0];
        /* Left alone, deliberately, same as the no-chain_params branch above:
         * `pages` is empty here, and trailing pages with nothing else on the
         * view is a shape no consumer expects. */
        if (!first) return { pages, fingerprint, warnings: warnings.concat("no levels") };
        warnings.push(`no "${rootKey}" level — starting at "${first}"`);
        rootKey = first;
    }

    /* Authored pages (a level's own knobs[]) dedupe by exact key list: 16
     * modules publish a `children` alias level that re-lists root's knobs, which
     * would otherwise render as a second identical page. Verified across the
     * fleet that no module has two genuinely distinct pages sharing a key list. */
    const renderedKnobSigs = new Set();
    const emitted = new Set();   /* every key placed on a grid page so far */

    /* Keys a level uses to drive its OWN page kind — the preset index, the item
     * selector, the mode. They are reachable through that page and must not also
     * appear as a knob: browsing 2427 presets by encoder is the thing the preset
     * page exists to avoid. */
    const selectorKeys = new Set();
    for (const lvl of Object.values(levels)) {
        if (!lvl || typeof lvl !== "object") continue;
        for (const f of ["list_param", "count_param", "name_param", "items_param", "select_param"]) {
            if (lvl[f]) selectorKeys.add(lvl[f]);
        }
    }
    if (hierarchy.mode_param) selectorKeys.add(hierarchy.mode_param);
    const visited = new Set();

    function emitLevel(levelKey, prefix) {
        const lvl = levels[levelKey];
        if (!lvl) return;
        if (lvl.visible_if && !isVisible(lvl.visible_if, lvl)) return;

        /* The walk root's grid page is always "Main", even when the level
         * declares a label. 16 modules would otherwise open on a page called
         * "Patch" / "Console" / "BOOM"; one consistent name for "where you land"
         * beats each module's own word for it. */
        const isRoot = levelKey === rootKey;
        const base = isRoot ? "Main" : nameOf(levelKey, lvl);
        const title = prefix ? `${prefix}/${base}` : base;

        /* Preset browser first — decided 2026-07-26. A level is routinely both
         * "the Main knob page" and "the preset browser" (obxd/root has 8 knobs,
         * 13 nav entries and the preset triple), and a knob is the wrong control
         * for minijv's 2427 or surge's 675 presets. */
        if (lvl.list_param && lvl.count_param) {
            pages.push({
                /* "Presets", not the level's name — the preset browser is a
                 * different thing from the knob page that shares its level. */
                kind: PAGE_PRESET,
                name: claimName(declaredName(levelKey, lvl) && !isRoot ? nameOf(levelKey, lvl) : "Presets"),
                level: levelKey,
                listParam: lvl.list_param, countParam: lvl.count_param,
                nameParam: lvl.name_param || "preset_name",
            });
        }

        /* Runtime-populated selection list (soundfonts, NAM models, JV
         * expansions): the page exists statically, its contents do not. */
        if (lvl.items_param) {
            pages.push({
                kind: PAGE_ITEMS, name: claimName(base), level: levelKey,
                itemsParam: lvl.items_param, selectParam: lvl.select_param || null,
                /* Where the module wants you AFTER choosing — the list editor
                 * honours this and so should the grid, or choosing a soundfont
                 * lands you somewhere the module did not intend. */
                navigateTo: lvl.navigate_to || null,
            });
        }

        /* Repeated elements declared once and multiplied by the host. The
         * level object travels with the page so the caller can resolve concrete
         * keys without re-reading the hierarchy (see child_key.mjs). */
        if (hasChildren(lvl) && childPickerNeeded(lvl, listedKeys, pickedFor)) {
            const _idxParam = childIndexParam(lvl);
            if (_idxParam) pickedFor.add(_idxParam);
            /*
             * A child selector IS an items page.
             *
             * Same gesture in every respect -- a list, a cursor, a current
             * marker, click to choose -- so it reuses PAGE_ITEMS rather than
             * being a second kind with its own state, its own jog handler and
             * its own copy of the renderer. The two module pickers drifted
             * apart exactly that way once already.
             *
             * The one difference is where the list comes from and what
             * choosing does: an ordinary items page reads `items_param` and
             * writes `select_param`, while this one DERIVES its list from the
             * level (count and label are declared, so it costs no IPC) and
             * commits to a local index that re-keys the level's own pages.
             * Both are expressed as fields on the page, not as a new kind.
             */
            /*
             * NAMED FOR WHAT IT SELECTS, not for its level.
             *
             * It inherited the level's name, so mrdrums' pad list was called
             * "Main" -- a list of pads under the name of the page you were
             * looking for. It is also the page BEFORE the ones it governs
             * (firstGrid lands past it, so you jog back to reach it), and a
             * page you arrive at by going backwards has to say what it is on
             * its own. Reported from the device: it should be "Selected Pad".
             */
            pages.push({
                kind: PAGE_ITEMS,
                name: claimName(`Selected ${lvl.child_label || "Item"}`),
                level: levelKey,
                childCount: childCount(lvl),
                childLabel: lvl.child_label || "Item",
                /* The SAME derived-list field the mode selector uses. One
                 * mechanism: the planner decides what the labels say, and
                 * itemsState never learns there are two kinds of source. */
                /* A DECLARED name wins over the generated one. Without this the
                 * page built its labels inline and never consulted the
                 * declaration, so a module that named its pads still saw
                 * "Pad 1 ... Pad 16" HERE while every other list showed "Kick".
                 * The unit test passed throughout because it called childLabel
                 * directly and never came through the planner.
                 *
                 * Only the NAME is shared: the trailing number stays 1-based
                 * here, because childLabel counts from child_index_base and
                 * minijv declares none -- so borrowing that too would renumber
                 * its picker from Part 1-8 to Part 0-7. See childName. */
                derivedLabels: Array.from(
                    { length: childCount(lvl) },
                    (_, i) => childName(lvl, i)
                        || `${lvl.child_label || "Item"} ${i + 1}`),
                childOf: levelKey,
                childLevel: lvl,
            });
        }

        /* Grid pages. knobs[] is the author's chosen eight, NOT their parameter
         * set: fleet-wide, 879 keys across 57 modules are listed in params[] and
         * on no knob. Rendering knobs[] alone would hide 28% of declared params
         * relative to the list editor, so the level's remaining editable params
         * follow its authored knobs as continuation pages. */
        /*
         * A SELECTOR key is dropped even when authored. This is the one
         * exception to "an authored knob is intent", and it is carried by
         * evidence rather than taste: fleet-wide, exactly two levels place a
         * selector on a knob — impressive-chords `preset_index` and breakbeat
         * `preset` — and in BOTH the knobs[] array is byte-identical to
         * params[]. Neither curated anything; they listed every param and the
         * selector happened to be first, so it took knob 1.
         *
         * Nobody does it deliberately, and the cost of honouring it is high:
         * the key already has its own browser page built from the same
         * list_param, so the knob is a duplicate — and a duplicate that cannot
         * work, because impressive-chords declares preset_index as int 0..500
         * against 52 presets, i.e. a knob that is ~90% dead travel and lands
         * on nothing.
         *
         * Reported from the device as "why is preset a knob on impressive
         * chords?".
         */
        const authored = knobKeys(lvl).filter(
            (k) => !isHiddenParam(lvl, k, isVisible) && !selectorKeys.has(k));
        /* Overflow keys are filtered further than authored ones: a knob the
         * author placed is otherwise intent and is honoured whatever it is
         * called, but a key we are pulling in from params[] has to earn its
         * cell.
         *
         *  - a selector param belongs to its own page kind, not a knob
         *    (also dropped above, for authored knobs)
         *  - `ui_`-prefixed keys are module UI state (ui_current_pad,
         *    ui_preset_path), not musical parameters — a naming convention the
         *    ecosystem already uses, and one Movy formalised independently
         *  - a key already placed elsewhere is not worth a second cell */
        const extra = paramKeys(lvl).filter((k) =>
            !authored.includes(k) &&
            !isHiddenParam(lvl, k, isVisible) &&
            !selectorKeys.has(k) &&
            !/^ui_/.test(k) &&
            !emitted.has(k));

        /* Dedupe applies to the AUTHORED key list only. 16 modules publish a
         * `children` alias level that re-lists root's knobs; suppressing the
         * whole level would also drop the extra params[] keys those aliases
         * carry (genera's scale/gen_mode, mrdrums' pad_* aliases — 10 modules,
         * 50 keys), which would silently reintroduce the §1 regression. So a
         * duplicate contributes its unseen extras and nothing else. */
        const sig = authored.join(" ");
        const dupAuthored = authored.length > 0 && renderedKnobSigs.has(sig);
        if (!dupAuthored && authored.length > 0) renderedKnobSigs.add(sig);

        const authoredKeys = dupAuthored ? [] : authored;
        const extraKeys = extra;

        /* Authored keys keep their exact 8-per-page grouping (see
         * balancedChunk); the remainder is spread evenly so a level with nine
         * keys yields 8 + 1 rather than an orphan page holding one control —
         * and a level with seventeen yields 8 + 5 + 4 rather than 8 + 8 + 1. */
        const parts = chunk(authoredKeys, perPage);
        const spill = perPage === Infinity ? 0 : authoredKeys.length % perPage;
        if (spill > 0 && extraKeys.length > 0) {
            /* A partly-filled authored page absorbs overflow up to 8. */
            const room = perPage - spill;
            const take = extraKeys.splice(0, room);
            parts[parts.length - 1] = parts[parts.length - 1].concat(take);
        }
        /* Unpaginated, the extras join the authored page rather than starting
         * one: the whole point is a single scrolling list per level, and
         * balancedChunk with no bound would hand back one array anyway — but as
         * a SECOND page, which is the thing being removed. */
        if (perPage === Infinity) {
            if (extraKeys.length) {
                if (parts.length) parts[parts.length - 1] = parts[parts.length - 1].concat(extraKeys);
                else parts.push(extraKeys.slice());
            }
        } else {
            for (const p of balancedChunk(extraKeys, perPage)) parts.push(p);
        }

        /*
         * A CHILD level's keys are TEMPLATES, not addresses.
         *
         * minijv's `part_selector` lists `partlevel`, but the DSP serves
         * `sram_part_<n>_partlevel` and gates on that prefix. The page carries
         * its LEVEL so the controller can resolve the concrete key for
         * whichever child the selector page has committed -- see
         * child_key.mjs and childResolve() in page_controller.mjs.
         *
         * These pages were suppressed entirely until that resolution existed,
         * because a cell addressing a key nobody serves looks completely alive
         * (chain_params gives it a label and a range) and does nothing at all
         * when turned. No error, no blank, no signal.
         */

        if (parts.length > 0) {
            for (const p of parts) for (const k of p) emitted.add(k);
            parts.forEach((keys) => {
                const pageName = claimName(title);
                pages.push({
                    kind: PAGE_KNOBS,
                    name: pageName,
                    level: levelKey, keys: alignKnobs(keys, pageName),
                    /* The level object travels with the page so the controller
                     * resolves concrete child keys without re-reading the
                     * hierarchy. Null for an ordinary level, which is what
                     * makes the resolution a no-op everywhere else. */
                    childLevel: hasChildren(lvl) ? lvl : null,
                    /*
                     * PER-PAGE cell labels, from this level's own inline
                     * entries. The page IS the context, so it is the right
                     * place to say what a cell is called: filter's env_amount
                     * wants "Amt" on the Envelope page and "Env Amt" on Main,
                     * and a param-level short_name cannot say both.
                     *
                     * It has to ride on the PAGE rather than the meta index,
                     * because that index is flat by design -- inline entries
                     * are keyed by the bare key across every level, so two
                     * levels naming the same key would silently overwrite each
                     * other. Collected here, where the level is still known.
                     */
                    shortNames: levelShortNames(lvl),
                    /* true when every key on this page came from knobs[] — i.e.
                     * the author placed it there. Pages built from params[] are
                     * false, including a page that mixes the two. */
                    authored: keys.every((k) => authored.includes(k)),
                });
            });
        }

        /* Menu LAST, after this level's grids. A preset browser goes first
         * because it is how you choose what the knobs are editing; a menu is
         * the opposite — Save/Delete are what you do when you have finished, so
         * landing on them is not where anyone wants to start. */
        if (Array.isArray(lvl.menu) && lvl.menu.length) {
            const menuEntries = mapMenuEntries(lvl.menu);
            if (menuEntries.length > 0) {
                pages.push({
                    kind: PAGE_MENU,
                    name: claimName(lvl.menu_label || `${base} Menu`),
                    level: levelKey,
                    entries: menuEntries,
                });
            }
        }
    }

    /* A param entry can carry its own visible_if. */
    function isHiddenParam(lvl, key, vis) {
        for (const p of ((lvl && lvl.params) || [])) {
            if (keyOf(p) === key && p && p.visible_if) return !vis(p.visible_if, lvl);
        }
        return false;
    }

    /* `transparent` marks a level reached through a `children` edge: it stands
     * in for its parent's menu rather than being a category, so it neither takes
     * nor contributes a name prefix. Without this every moog page would read
     * "main/Oscillator 1". A `params` nav edge DOES contribute one — that is
     * what keeps minijv's four "Filter" pages apart as Tone 1/Filter etc. */
    function visit(levelKey, prefix, transparent) {
        if (visited.has(levelKey)) return;
        visited.add(levelKey);
        const lvl = levels[levelKey];
        if (!lvl) return;
        if (lvl.visible_if && !isVisible(lvl.visible_if, lvl)) return;

        emitLevel(levelKey, prefix);

        /* Root's children carry no prefix: they are the module's top-level
         * categories, so "Filter" beats "Root/Filter" — and the header is only
         * 128 px wide, shared with the module name. Prefixes start one level
         * down, which is exactly where they earn their keep (minijv would
         * otherwise show four pages called "Filter").
         *
         * A level declaring no knobs of its own is a MENU, not a category — it
         * exists to point at other levels. It contributes no prefix either, so
         * minijv's tone_selector ("Edit Tones") gives "Tone 1" rather than
         * "EditT/Tone 1", while tone1 itself still prefixes its children as
         * "Tone1/Wave". */
        const isMenuLevel = knobKeys(lvl).length === 0 && !lvl.items_param;
        const childPrefix = (levelKey === rootKey || isMenuLevel) ? prefix
            : transparent ? prefix
            : levelNameToPrefix(nameOf(levelKey, lvl));
        /* Both edges, always: a level with knobs can still own sub-levels
         * (dexed's Operators, forge's Voice, minijv's tone1). */
        for (const p of ((lvl.params) || [])) {
            const target = levelOf(p);
            if (target) visit(target, childPrefix, false);
        }
        const kid = childOf(lvl);
        if (kid) visit(kid, prefix, true);
    }

    /* Root's own knobs become the first grid page, named for the root level. */
    visit(rootKey, null, false);

    /* Levels no edge reaches would otherwise be permanently invisible.
     *
     * This used to name minijv's performance/perf_main/part_selector as the
     * case it existed for. It no longer does: those are reached from the
     * `performance` mode root, and hauling them in here is what stopped the
     * mode from gating anything. `inactiveModeLevels` is the difference
     * between an orphan and somebody else's tree. */
    for (const key of Object.keys(levels)) {
        if (visited.has(key)) continue;
        if (inactiveModeLevels.has(key)) continue;
        const lvl = levels[key];
        if (knobKeys(lvl).length === 0 && paramKeys(lvl).length === 0 && !lvl.items_param && !lvl.list_param) continue;
        visit(key, null, false);
    }

    return { pages: appendTrailing(pages, claimName), fingerprint, warnings,
             conditionKeys, realigned };
}

/**
 * Physical knob (0-7) → key on the given grid page, or null for an empty slot.
 * Exported so a caller that owns input (the shadow UI, or a sequencer capturing
 * parameter locks) can route encoder events without the planner touching MIDI.
 */
export function pageSlotKeys(page) {
    const out = new Array(KNOBS_PER_PAGE).fill(null);
    if (!page || page.kind !== PAGE_KNOBS) return out;
    for (let i = 0; i < Math.min(page.keys.length, KNOBS_PER_PAGE); i++) out[i] = page.keys[i];
    return out;
}
