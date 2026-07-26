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
import { buildMetaIndex, inferFromValue, isTurnable, KIND_OPAQUE } from "./param_meta.mjs";
import { renderPage, LAYOUT_DIAL } from "./render_page.mjs";
import { step, stepLevel, reanchor, firstGrid, jumpIndex, groupIndex } from "./page_nav.mjs";
import { knobInit, knobTick, knobConfigFromMeta } from "../knob_engine.mjs";
import { formatParamForSet } from "../param_format.mjs";
import { announcePage, announceTouch, announceTurn, announcePageContents } from "./announce_page.mjs";

/** Ticks a key ignores incoming reads after being turned (~200 ms at 44 Hz). */
export const SETTLE_TICKS = 9;

export function createController(io = {}) {
    const getParam = io.getParam || (() => null);
    const setParam = io.setParam || (() => {});
    const announce = io.announce || (() => {});
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

    /* ------------------------------------------------------------ reading */

    /**
     * One read per tick, cycling the current page. Values arrive over several
     * frames rather than stalling one — the whole point of the cursor.
     */
    function tick() {
        s.tickCount++;
        const p = page();
        if (!p || p.kind !== PAGE_KNOBS || p.keys.length === 0) return null;

        const key = p.keys[s.cursor % p.keys.length];
        s.cursor = (s.cursor + 1) % p.keys.length;
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

    /** Jog: pages. With shift: whole levels, skipping continuations. */
    function onJog(delta, { shift = false } = {}) {
        if (!s.pages.length || delta === 0) return s.pageIndex;
        const before = s.pageIndex;
        s.pageIndex = shift ? stepLevel(s.pages, s.pageIndex, delta)
                            : step(s.pages, s.pageIndex, delta);
        if (s.pageIndex !== before) {
            s.cursor = 0;
            s.touched = -1;
            announcePageChange();
        }
        return s.pageIndex;
    }

    /** Jump straight to a page (from the index or group picker). */
    function goToPage(index) {
        if (index === s.pageIndex) return s.pageIndex;
        s.pageIndex = Math.max(0, Math.min(s.pages.length - 1, index));
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
    function onKnobTurn(slot, direction, nowMs) {
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
        const value = knobTick(st, knobConfigFromMeta(meta), direction, t);
        const wire = formatParamForSet(value, meta);

        s.values[key] = wire;
        s.settleUntil[key] = s.tickCount + SETTLE_TICKS;
        setParam(fullKey(key), wire);
        announce(announceTurn(meta, wire));
        return wire;
    }

    /** Capacitive touch. Down announces the full name and value. */
    function onKnobTouch(slot, down) {
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

    function takePending() {
        const p = s.pending;
        s.pending = null;
        return p;
    }

    /* --------------------------------------------------------- presentation */

    function setLayout(layout) { s.layout = layout; }
    function setReveal(on) { s.revealValues = !!on; }
    function setDecorations(d) { s.decorations = d || null; }

    function render(ctx, { title, rect } = {}) {
        renderPage(ctx, {
            page: page(), metaIndex: s.metaIndex, values: s.values,
            title: title || "", pageIndex: s.pageIndex, pageCount: s.pages.length,
            touched: s.touched, decorations: s.decorations,
            layout: s.layout, revealValues: s.revealValues, rect,
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
        setLayout, setReveal, setDecorations, render, announceContents,
        get state() { return s; },
        get page() { return page(); },
        get pages() { return s.pages; },
        get pageIndex() { return s.pageIndex; },
        keyAt, metaAt,
        jumpIndex: () => jumpIndex(s.pages),
        groupIndex: () => groupIndex(s.pages),
    };
}

function parse(raw) {
    if (!raw) return null;
    try { return JSON.parse(raw); } catch { return null; }
}
