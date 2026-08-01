/**
 * page_controller.mjs — the interaction model for a knob page.
 *
 * This is the part that would normally live inside shadow_ui.js as a few
 * hundred lines of view state, and therefore be untestable without a Move. It
 * is here instead, pure, with every device call injected:
 *
 *   getParam(fullKey)        -> string|null
 *   setParam(fullKey, value) -> void
 *   announce(text)           -> void          (optional)
 *   isModulated(fullKey)     -> boolean       (optional)
 *   now()                    -> ms            (optional, injectable clock)
 *
 * What is left for the real binding is genuinely thin: route MIDI to the
 * handlers below, call `tick()` once a frame, and call `render()`. Everything
 * with a decision in it — knob feel, staggered reads, when to rebuild, what to
 * announce — is testable here.
 *
 * Two behaviours carry most of the risk and both are pinned by tests:
 *
 *   Staggered reads. Eight live values per page is eight IPC round trips.
 *   Movy measured bulk refresh blocking ~186 ms per cycle. This reads ONE param
 *   per tick, cycling the current page.
 *
 *   Write-back suppression. A read issued before a knob turn lands after it and
 *   would drag the value backwards. Reads are ignored for a key while it is
 *   being turned and for a short settling window afterwards.
 */

import { planPages, PAGE_KNOBS } from "./page_plan.mjs";
import { buildMetaIndex, inferFromValue, isTurnable, KIND_ENUM, KIND_OPAQUE } from "./param_meta.mjs";
import { renderPage, renderPicker, renderHint, LAYOUT_DIAL } from "./render_page.mjs";
import { step, stepLevel, reanchor, firstGrid, jumpIndex, groupIndex } from "./page_nav.mjs";
import { knobInit, knobTick, knobConfigFromMeta } from "../knob_engine.mjs";
import { formatParamForSet } from "../param_format.mjs";
import { announcePage, announceTouch, announceTurn, announcePageContents } from "./announce_page.mjs";

/** Ticks a key ignores incoming reads after being turned (~200 ms at 44 Hz). */
export const SETTLE_TICKS = 9;

/** How many times a page will re-read the contract waiting for late metadata. */
export const META_RETRY_LIMIT = 8;
/** Ticks between those attempts (~1 s at the shadow UI's 344 Hz tick).
 *  Paced by wall-clock rather than by page sweeps: an 8-key page wraps every
 *  9 ticks, which would burn the whole retry budget in under two seconds —
 *  long before a module that loads a ROM has finished. */
export const META_RETRY_INTERVAL_TICKS = 344;

export function createController(io = {}) {
    const getParam = io.getParam || (() => null);
    const setParam = io.setParam || (() => {});
    const announce = io.announce || (() => {});
    /* Optional: is this param currently driven by a modulation source? The
     * library cannot answer that — it is host state — so it is injected, and
     * defaults to "no" for callers that have no modulation. */
    const isModulated = io.isModulated || (() => false);
    const now = io.now || (() => Date.now());

    const s = {
        slot: 0,
        component: "synth",
        prefix: "synth",
        pages: [],
        pageIndex: 0,
        fingerprint: null,
        metaIndex: null,
        layout: LAYOUT_DIAL,
        revealValues: false,
        touched: -1,
        values: Object.create(null),
        decorations: null,
        /* staggered read cursor */
        cursor: 0,
        /* key -> tick at which reads may resume */
        settleUntil: Object.create(null),
        tickCount: 0,
        knobStates: Object.create(null),
        /* the caller acts on these; the controller never opens a screen itself */
        pending: null,
        /* Page picker: the answer to 76 pages. Open, jog to scroll, click to
         * jump. Held here rather than in the host because it is navigation over
         * the page set, which is what this module is for. */
        pickerOpen: false,
        pickerIndex: 0,
        pickerEntries: [],
        /* First-run gesture hint. Shown until the user does literally anything,
         * then gone for the session — a timer would either be too short to read
         * or long enough to be in the way. */
        hintLines: null,
        hintShown: false,
        /* Out-of-band status the UI wants but no module declares in
         * chain_params. Folded into the read cursor rather than polled
         * separately, so it costs one slot in the rotation, not a frame. */
        presetName: null,
        /* Metadata that arrives after the module reports ready. osirus loads a
         * ROM asynchronously and publishes `rom_index` as ["(loading)"]; baked
         * once at load time, that enum would read "(loading)" for the rest of
         * the session. Re-resolution is bounded and latching — see maybeResettle. */
        metaRetries: 0,
        metaSettled: false,
        /* Per-section memory of the sub-page you were last on. Elektron's page
         * buttons work this way — pressing [FLTR] returns you to the FLTR page
         * you were using, not to FLTR 1 — and it matters most on the modules
         * where it is most tedious to get back (minijv's tone subtrees are 15
         * pages each). Applies to SECTION jumps only; a fine jog still steps
         * linearly, or you could never walk the set in order. */
        sectionMemory: Object.create(null),
    };

    const fullKey = (key) => `${s.prefix}:${key}`;
    const page = () => s.pages[s.pageIndex] || null;
    const keyAt = (slot) => {
        const p = page();
        return p && p.kind === PAGE_KNOBS ? (p.keys[slot] || null) : null;
    };
    const metaAt = (slot) => {
        const k = keyAt(slot);
        return k && s.metaIndex ? s.metaIndex.getOrGuess(k) : null;
    };

    /**
     * Point the controller at a component and build its page set.
     * Safe to call repeatedly — it rebuilds only when the declared contract
     * actually changed, and keeps the user's place when it does.
     */
    function load({ slot = 0, component = "synth", prefix, mode, visible } = {}) {
        s.lastLoadOpts = { mode, visible };
        s.slot = slot;
        s.component = component;
        s.prefix = prefix || component;

        const hierarchy = parse(getParam(`${s.prefix}:ui_hierarchy`));
        const chainParams = parse(getParam(`${s.prefix}:chain_params`));
        const planned = planPages({ hierarchy, chainParams, mode, visible });

        if (planned.fingerprint === s.fingerprint) return false;

        const oldPages = s.pages;
        const oldIndex = s.pageIndex;
        s.pages = planned.pages;
        s.fingerprint = planned.fingerprint;
        s.metaIndex = buildMetaIndex({ hierarchy, chainParams });
        s.values = Object.create(null);
        s.cursor = 0;
        s.metaRetries = 0;
        s.metaSettled = false;
        s.knobStates = Object.create(null);
        /* A rebuild after a module finishes loading shifts every index, so land
         * by name rather than by position; a first load lands on a grid. */
        s.pageIndex = oldPages.length ? reanchor(oldPages, oldIndex, s.pages) : firstGrid(s.pages);
        announcePageChange();
        return true;
    }

    /** Poll for a contract that changed underneath us (async ROM/sample loads). */
    function reloadIfChanged(opts) {
        return load({ slot: s.slot, component: s.component, prefix: s.prefix, ...opts });
    }

    /**
     * Is any enum on the current page still showing a placeholder?
     *
     * A module that is still loading publishes a stand-in option set — exactly
     * one entry wrapped in parentheses, "(loading)" — or no options at all.
     * Those are the two shapes worth waiting for; anything else is a real,
     * settled declaration.
     */
    function metaUnsettled() {
        const p = page();
        if (!p || p.kind !== PAGE_KNOBS || !s.metaIndex) return false;
        for (const key of p.keys) {
            const meta = s.metaIndex.getOrGuess(key);
            if (meta.kind !== KIND_ENUM) continue;
            const o = meta.options;
            if (!o) return true;
            if (o.length === 1 && /^\(.*\)$/.test(String(o[0]))) return true;
        }
        return false;
    }

    /**
     * Bounded, latching re-resolve of late metadata.
     *
     * Costs one contract read per interval while something is unsettled and
     * NOTHING once it settles or the retry budget runs out — a module whose
     * enum legitimately reads "(none)" must not make us poll forever.
     */
    function maybeResettle(reload) {
        if (s.metaSettled || s.metaRetries >= META_RETRY_LIMIT) return false;
        if (!metaUnsettled()) { s.metaSettled = true; return false; }
        s.metaRetries++;
        return reload();
    }

    /* ------------------------------------------------------------ reading */

    /**
     * One read per tick, cycling the current page. Values arrive over several
     * frames rather than stalling one — the whole point of the cursor.
     */
    function tick() {
        s.tickCount++;
        const p = page();
        if (!p || p.kind !== PAGE_KNOBS || p.keys.length === 0) return null;

        /* One extra stop in the rotation reads the preset name, which a
         * hardware synth would put in its display and which no module declares
         * as a param. */
        const stops = p.keys.length + 1;
        const at = s.cursor % stops;
        s.cursor = (s.cursor + 1) % stops;

        /* Give late metadata a chance to arrive, on a wall-clock cadence. */
        if (s.tickCount % META_RETRY_INTERVAL_TICKS === 0) {
            maybeResettle(() => reloadIfChanged(s.lastLoadOpts));
        }

        if (at === p.keys.length) {
            const pn = getParam(`${s.prefix}:preset_name`);
            s.presetName = (pn && pn.length) ? pn : null;
            return null;
        }
        const key = p.keys[at];
        if (!key) return null;

        /* Do not clobber a value the user is actively turning. */
        if ((s.settleUntil[key] || 0) > s.tickCount) return null;

        const raw = getParam(fullKey(key));
        if (raw === null || raw === undefined) return null;

        /* First successful read repairs a guessed range, once. */
        const meta = s.metaIndex.getOrGuess(key);
        if (meta.guessed) {
            const patch = inferFromValue(meta, raw);
            if (patch) Object.assign(meta, patch);
            delete meta.guessed;
        }
        s.values[key] = raw;
        return key;
    }

    /* ------------------------------------------------------------- input */

    /**
     * Open the page picker: one entry per section rather than per page, since a
     * list of 76 pages is the same chore in a different shape. minijv folds to
     * under 25 entries this way.
     */
    function openPicker() {
        s.pickerEntries = groupIndex(s.pages);
        if (!s.pickerEntries.length) return false;
        /* Start on the section you are already in. */
        let cur = 0;
        for (let i = 0; i < s.pickerEntries.length; i++) {
            if (s.pickerEntries[i].index <= s.pageIndex) cur = i;
        }
        s.pickerIndex = cur;
        s.pickerOpen = true;
        announce(`Sections, ${s.pickerEntries[cur].name}, ${cur + 1} of ${s.pickerEntries.length}`);
        return true;
    }

    function closePicker() {
        if (!s.pickerOpen) return false;
        s.pickerOpen = false;
        announcePageChange();
        return true;
    }

    /** Commit the highlighted section and return to the grid. */
    function pickerSelect() {
        if (!s.pickerOpen) return false;
        const entry = s.pickerEntries[s.pickerIndex];
        s.pickerOpen = false;
        if (entry) goToPage(entry.index);
        return true;
    }

    /* Remember where you were within the current section. */
    function rememberSection() {
        const p = page();
        if (p && p.level) s.sectionMemory[p.level] = s.pageIndex;
    }

    /* Landing on a section: return to the sub-page last used there. */
    function restoreSection(index) {
        const p = s.pages[index];
        if (!p || !p.level) return index;
        const remembered = s.sectionMemory[p.level];
        if (remembered === undefined) return index;
        const rp = s.pages[remembered];
        return (rp && rp.level === p.level) ? remembered : index;
    }

    /** Jog: pages. With shift: whole levels, skipping continuations. */
    function onJog(delta, { shift = false } = {}) {
        if (s.hintLines) dismissHint();
        if (s.pickerOpen) {
            const n = s.pickerEntries.length;
            if (!n) return s.pageIndex;
            const before = s.pickerIndex;
            s.pickerIndex = Math.max(0, Math.min(n - 1, s.pickerIndex + (delta > 0 ? 1 : -1)));
            if (s.pickerIndex !== before) {
                const e = s.pickerEntries[s.pickerIndex];
                announce(`${e.name}, ${s.pickerIndex + 1} of ${n}`);
            }
            return s.pageIndex;
        }
        if (!s.pages.length || delta === 0) return s.pageIndex;
        const before = s.pageIndex;
        rememberSection();
        s.pageIndex = shift ? restoreSection(stepLevel(s.pages, s.pageIndex, delta))
                            : step(s.pages, s.pageIndex, delta);
        if (s.pageIndex !== before) {
            s.cursor = 0;
            s.touched = -1;
            announcePageChange();
        }
        return s.pageIndex;
    }

    /** Jump straight to a page (from the index or group picker). */
    function goToPage(index, { remember = true } = {}) {
        if (index === s.pageIndex) return s.pageIndex;
        rememberSection();
        const target = Math.max(0, Math.min(s.pages.length - 1, index));
        s.pageIndex = remember ? restoreSection(target) : target;
        s.cursor = 0;
        s.touched = -1;
        announcePageChange();
        return s.pageIndex;
    }

    /**
     * A physical knob moved. Applies the shared knob_engine so a value moves
     * identically here and in the list editor, writes through, and holds off
     * reads for that key until it settles.
     */
    function onKnobTurn(slot, direction, nowMs, { fine = false } = {}) {
        if (s.hintLines) dismissHint();
        const key = keyAt(slot);
        if (!key) return null;
        const meta = s.metaIndex.getOrGuess(key);
        /* A filepath or canvas cannot be turned — it opens. Swallow the motion
         * rather than writing nonsense into it. */
        if (!isTurnable(meta)) return null;

        const t = nowMs === undefined ? now() : nowMs;
        let st = s.knobStates[key];
        if (!st) {
            const current = s.values[key] !== undefined ? Number(s.values[key]) : Number(getParam(fullKey(key)));
            st = s.knobStates[key] = knobInit(isFinite(current) ? current : 0);
        }
        /* Fine adjust: Elektron's [FUNC]+encoder. Holding shift already reveals
         * every value, so precision mode and "show me the numbers" are the same
         * gesture — which is what you want when you are chasing a value.
         *
         * Only floats have a finer step to give. An int already moves in whole
         * units and an enum in whole options; there is nothing below that, and
         * pretending otherwise would just make them feel broken under shift. */
        const cfg = knobConfigFromMeta(meta);
        const canRefine = fine && meta.type === "float";
        const value = knobTick(st, canRefine ? { ...cfg, step: (cfg.step || 0.01) / 10 } : cfg,
                               direction, t);
        const wire = formatParamForSet(value, meta);

        s.values[key] = wire;
        s.settleUntil[key] = s.tickCount + SETTLE_TICKS;
        setParam(fullKey(key), wire);
        announce(announceTurn(meta, wire));
        return wire;
    }

    /** Capacitive touch. Down announces the full name and value. */
    function onKnobTouch(slot, down) {
        if (s.hintLines) dismissHint();
        /* Reaching for a knob is an unambiguous "I want the grid", so it
         * dismisses the picker rather than leaving you in a modal you have to
         * back out of first. */
        if (down && s.pickerOpen) closePicker();
        if (!down) {
            if (s.touched === slot) s.touched = -1;
            return;
        }
        s.touched = slot;
        const key = keyAt(slot);
        const meta = metaAt(slot);
        const dec = s.decorations ? s.decorations[slot] : null;
        announce(announceTouch(meta, key ? s.values[key] : null, slot, dec));
    }

    /**
     * Click on a knob's cell. A turnable param has nothing to open; an opaque
     * one (filepath, canvas, wav_position, string) asks the caller to open the
     * editor the list view already has. The controller never opens it itself —
     * that screen belongs to the host.
     */
    function onClick(slot) {
        const key = keyAt(slot);
        const meta = metaAt(slot);
        if (!key || !meta || meta.kind !== KIND_OPAQUE) return null;
        s.pending = { action: "open", key, fullKey: fullKey(key), meta };
        return s.pending;
    }

    /**
     * Reset a knob's param to the default its module declared. 744 params across
     * 39 modules declare one, and there is otherwise no way back to it short of
     * reloading the preset.
     *
     * Returns false when the param declares no default, so the caller can say
     * so rather than silently doing nothing.
     */
    function resetToDefault(slot) {
        const key = keyAt(slot);
        const meta = metaAt(slot);
        if (!key || !meta || meta.default === undefined || meta.default === null) return false;
        if (!isTurnable(meta)) return false;

        const wire = formatParamForSet(meta.default, meta);
        s.values[key] = wire;
        s.settleUntil[key] = s.tickCount + SETTLE_TICKS;
        delete s.knobStates[key];       /* next turn starts from the new value */
        setParam(fullKey(key), wire);
        announce(`${meta.label || key}, default, ${announceTurn(meta, wire)}`);
        return true;
    }

    function takePending() {
        const p = s.pending;
        s.pending = null;
        return p;
    }

    /* --------------------------------------------------------- presentation */

    /** Arm the first-run hint. Ignored once it has been shown and dismissed. */
    function showHint(lines, title) {
        if (s.hintShown) return false;
        s.hintLines = { lines, title };
        return true;
    }

    function dismissHint() {
        if (!s.hintLines) return false;
        s.hintLines = null;
        s.hintShown = true;
        return true;
    }

    function setLayout(layout) { s.layout = layout; }
    function setReveal(on) { s.revealValues = !!on; }
    function setDecorations(d) { s.decorations = d || null; }

    function render(ctx, { title, rect } = {}) {
        if (s.hintLines) {
            renderPage(ctx, {
                page: page(), metaIndex: s.metaIndex, values: s.values,
                title: title || "", pageIndex: s.pageIndex, pageCount: s.pages.length,
                touched: -1, layout: s.layout, rect,
            });
            renderHint(ctx, { rect, lines: s.hintLines.lines, title: s.hintLines.title });
            return;
        }
        if (s.pickerOpen) {
            renderPicker(ctx, { rect, entries: s.pickerEntries, index: s.pickerIndex, title: "Sections" });
            return;
        }
        renderPage(ctx, {
            page: page(), metaIndex: s.metaIndex, values: s.values,
            title: title || "", pageIndex: s.pageIndex, pageCount: s.pages.length,
            touched: s.touched, decorations: s.decorations,
            layout: s.layout, revealValues: s.revealValues, rect,
            modulated: (key) => isModulated(fullKey(key)),
        });
    }

    /** Read the current page aloud — the gesture that replaces a glance. */
    function announceContents() {
        announce(announcePageContents(page(), s.metaIndex, s.values, s.decorations));
    }

    function announcePageChange() {
        announce(announcePage(page(), s.pageIndex, s.pages.length));
    }

    return {
        load, reloadIfChanged, tick,
        onJog, goToPage, onKnobTurn, onKnobTouch, onClick, takePending,
        openPicker, closePicker, pickerSelect, showHint, dismissHint, resetToDefault,
        get pickerOpen() { return s.pickerOpen; },
        get pickerEntries() { return s.pickerEntries; },
        get pickerIndex() { return s.pickerIndex; },
        setLayout, setReveal, setDecorations, render, announceContents,
        get state() { return s; },
        get page() { return page(); },
        get pages() { return s.pages; },
        get pageIndex() { return s.pageIndex; },
        /** The loaded preset's name, once the cursor has read it. */
        get presetName() { return s.presetName; },
        get metaIndex() { return s.metaIndex; },
        keyAt, metaAt,
        jumpIndex: () => jumpIndex(s.pages),
        groupIndex: () => groupIndex(s.pages),
    };
}

function parse(raw) {
    if (!raw) return null;
    try { return JSON.parse(raw); } catch { return null; }
}
