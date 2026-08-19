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
import { renderPageMovy, LAYOUT_MOVY } from "./render_page_movy.mjs";
import { resolveViz } from "./viz.mjs";

export { LAYOUT_MOVY };
import { step, stepLevel, reanchor, firstGrid, jumpIndex, groupIndex } from "./page_nav.mjs";
import { knobInit, knobTick, knobConfigFromMeta } from "../knob_engine.mjs";
import { movyKnobInit, movyKnobTick } from "./movy_knob.mjs";
import { formatParamForSet } from "../param_format.mjs";
import { announcePage, announceTouch, announceTurn, announcePageContents } from "./announce_page.mjs";

/** Ticks a key ignores incoming reads after being turned (~200 ms at 44 Hz). */
export const SETTLE_TICKS = 9;

/**
 * Minimum gap between announcements for the SAME key while it is being
 * turned continuously. A fast physical spin decodes to one MIDI CC message
 * per detent — measured on device at up to ~286/s during a fast Braids turn
 * — and every one of those was reaching `announce()`, which always writes to
 * shared memory and bumps a sequence number for the screen-reader consumer
 * to pick up (`host_send_screenreader`, `src/shadow/shadow_ui.c`) whether or
 * not TTS is actually speaking. No one can follow 286 announcements a
 * second, sighted or not, and competing with that many per-detent writes for
 * the same tick budget as rendering was the real cause behind the frame rate
 * dropping under a fast turn (17fps idle -> 5fps while flooded) — this
 * throttle is a genuine UX fix on its own merits, and it happens to be the
 * perf fix too. */
export const ANNOUNCE_THROTTLE_MS = 120;

/**
 * Minimum gap between setParam WRITES for the same key while it is being
 * turned continuously.
 *
 * Measured on device: a fast physical spin decodes to 250-320 MIDI CC
 * messages/second (one per detent), and — confirmed by bypassing it —
 * `setParam` per detent is what was dropping the grid's own redraw rate from
 * ~17fps to 5fps under that load, not rendering (every draw primitive
 * measures near-zero) and not `announce()` (throttling that alone changed
 * nothing). 50 writes/sec is already finer than a human ear or a knob's own
 * declared `step` resolution needs during a fast sweep — the value the
 * screen shows and the value used for the next detent's math (`s.values`,
 * `knobStates`) update on EVERY detent regardless; only the outbound
 * `setParam` IPC call is paced. A write that misses this window is not
 * dropped — see `pendingWrite` below — it is caught by the next tick or by
 * release, so the final settled value always reaches the device exactly. */
export const SETPARAM_THROTTLE_MS = 20;

/**
 * How many modulated params get a live re-read per tick.
 *
 * The staggered cursor exists because eight values is eight IPC round trips,
 * and it works because a human turns one knob at a time. A modulation source
 * breaks that: those values move on their own, continuously, and on the shared
 * cursor each one refreshes only every `stops` ticks — about 5Hz, which
 * against a 1/8-note LFO is undersampled enough that the dot wanders instead
 * of sweeping.
 *
 * Modulated keys therefore get their own lane. Typically one or two params on
 * a page are modulated, so they land at 42Hz and 21Hz respectively — smooth.
 * The cap is what keeps a pathological page honest: at ~2.8ms a read, three is
 * ~8.4ms against a 23.8ms frame, and beyond that a fully-modulated page would
 * spend the whole budget on IPC. Past the cap it degrades to round-robin
 * rather than dropping frames.
 *
 * The real fix for the many-modulated case is publishing effective values in
 * shared memory the way slot mute/solo now is — the shim already computes
 * them every block. This is the version that needs no new SHM contract, and
 * it is worth measuring whether it is enough before building that.
 */
export const MOD_FAST_READS_PER_TICK = 3;

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
    /* Graphics default on; a caller can pass `enableViz: false` to keep the
     * plain grid (a tool that wants every cell individually addressable), and
     * `vizOverrides` to correct a wrong detector guess without a module
     * release — see viz.mjs resolveViz. */
    const vizEnabled = io.enableViz !== false;
    const vizOverrides = io.vizOverrides || null;

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
        /* key -> last-read modulation flag, refreshed on the read cursor
         * rather than per cell per draw. See tick(). */
        modCache: Object.create(null),
        /* key -> live modulated ("effective") value, for the dot on the arc.
         * Only modulated keys are in here, and they get their own fast lane in
         * tick() because they are the only values that move on their own. */
        modValues: Object.create(null),
        /* Rotates over the modulated keys, so the fast lane stays bounded. */
        modCursor: 0,
        /* key -> tick at which reads may resume */
        settleUntil: Object.create(null),
        tickCount: 0,
        knobStates: Object.create(null),
        /* key -> ms of the last announce() for that key — see ANNOUNCE_THROTTLE_MS */
        lastAnnounceMs: Object.create(null),
        /* key -> ms of the last setParam() WRITE for that key, and key -> the
         * latest computed wire value still waiting to be written because it
         * arrived inside the throttle window — see SETPARAM_THROTTLE_MS. */
        lastWriteMs: Object.create(null),
        pendingWrite: Object.create(null),
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
        /* Param keys a visible_if condition reads. A condition is driven by a
         * VALUE, which moves without the declared contract moving, so the
         * fingerprint cannot see it — these are watched explicitly instead.
         * Cheaper and more exact than polling: only these keys can change what
         * is visible, and we already read every key on the page. */
        conditionKeys: new Set(),
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
        /* Retained so a visibility re-plan costs no extra device reads. */
        s.hierarchy = hierarchy;
        s.chainParams = chainParams;

        if (planned.fingerprint === s.fingerprint) return false;

        const oldPages = s.pages;
        const oldIndex = s.pageIndex;
        s.pages = planned.pages;
        s.fingerprint = planned.fingerprint;
        s.metaIndex = buildMetaIndex({ hierarchy, chainParams });
        s.conditionKeys = planned.conditionKeys || new Set();
        /* A rebuild mid-turn must not silently drop a throttled write that
         * hasn't reached the device yet. */
        flushDueWritesUnconditionally();
        s.values = Object.create(null);
        s.cursor = 0;
        s.metaRetries = 0;
        s.metaSettled = false;
        s.knobStates = Object.create(null);
        s.lastWriteMs = Object.create(null);
        s.pendingWrite = Object.create(null);
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
    /**
     * Catches the case a per-detent flush in onKnobTurn cannot: the hand
     * pauses mid-turn (still touching, so no release event either) with a
     * value sitting in pendingWrite from the last detent before the pause.
     * Nothing else would ever write it out. Cheap when there is nothing
     * pending — the common case — since it is only object-key iteration.
     */
    function flushDueWrites() {
        const t = now();
        for (const key in s.pendingWrite) {
            if (t - (s.lastWriteMs[key] || 0) < SETPARAM_THROTTLE_MS) continue;
            setParam(fullKey(key), s.pendingWrite[key]);
            s.lastWriteMs[key] = t;
            delete s.pendingWrite[key];
        }
    }

    /** Every pending write, ignoring the throttle window — a rebuild (module
     * swap, visible_if re-plan) must never silently drop one. */
    function flushDueWritesUnconditionally() {
        for (const key in s.pendingWrite) {
            setParam(fullKey(key), s.pendingWrite[key]);
        }
    }

    function tick() {
        s.tickCount++;
        flushDueWrites();
        const p = page();
        if (!p || p.kind !== PAGE_KNOBS || p.keys.length === 0) return null;

        refreshModulatedValues(p);

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

        /* Refresh this key's modulation flag on the SAME rotation as its value.
         *
         * The renderer asks `modulated(key)` for every cell of every draw, and
         * each of those was a synchronous round trip: measured on device, the
         * `<key>:modulated` reads were 3.5 of the grid's 7.1 reads per tick —
         * half of them — for an indicator that only changes when the user
         * edits a modulation routing. (Worse on a full page: eight knobs is
         * eight round trips per draw, and the no-`:modulated` fallback path
         * costs up to three reads each.)
         *
         * On the cursor it costs one read per tick and the whole page is
         * current within `stops` ticks — under 0.2s, for a tick mark. */
        s.modCache[key] = !!isModulated(fullKey(key));

        /* For a modulated key the plain key returns the EFFECTIVE value — what
         * the source is currently driving it to — and that belongs to the dot.
         * The pointer wants the base, so ask for it directly. Same one read on
         * the cursor either way; the extra cost of showing both values is the
         * fast lane above, not this.
         *
         * `:base` is served by chain_mod_get_base_for_subkey and only exists
         * while a target is active, so fall back rather than blank the knob if
         * the flag and the target ever disagree. */
        let raw = null;
        if (s.modCache[key]) raw = getParam(fullKey(key) + ":base");
        if (raw === null || raw === undefined) raw = getParam(fullKey(key));
        if (raw === null || raw === undefined) return null;

        /* First successful read repairs a guessed range, once. */
        const meta = s.metaIndex.getOrGuess(key);
        if (meta.guessed) {
            const patch = inferFromValue(meta, raw);
            if (patch) Object.assign(meta, patch);
            delete meta.guessed;
        }
        /* A change to a key that gates visibility re-plans the page set: the
         * params it hides or reveals are not otherwise reachable. */
        const changed = s.values[key] !== raw;
        s.values[key] = raw;
        if (changed && s.conditionKeys.has(key)) {
            const oldPages = s.pages, oldIndex = s.pageIndex;
            const planned = planPages({
                hierarchy: s.hierarchy, chainParams: s.chainParams,
                mode: s.lastLoadOpts && s.lastLoadOpts.mode,
                visible: s.lastLoadOpts && s.lastLoadOpts.visible,
            });
            if (planned.pages.length !== oldPages.length ||
                planned.pages.some((p, i) => (p.keys || []).join() !== ((oldPages[i] || {}).keys || []).join())) {
                s.pages = planned.pages;
                s.conditionKeys = planned.conditionKeys || new Set();
                s.pageIndex = reanchor(oldPages, oldIndex, s.pages);
                s.cursor = 0;
            }
        }
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

    /* Keyed on level+kind, not level alone: a level can carry more than one
     * page kind sharing one level key (braids' root is both the "Presets"
     * PAGE_PRESET browser and the "Main" PAGE_KNOBS grid) — a level-only key
     * let memory of one hijack a jump to the other. Picking "Presets" from
     * the section list landed back on "Main" because sectionMemory["root"]
     * held the knobs page and restoreSection only checked level. */
    function sectionKey(p) { return p ? `${p.level} ${p.kind}` : null; }

    /* Remember where you were within the current section. */
    function rememberSection() {
        const p = page();
        const key = sectionKey(p);
        if (p && p.level && key) s.sectionMemory[key] = s.pageIndex;
    }

    /* Landing on a section: return to the sub-page last used there. */
    function restoreSection(index) {
        const p = s.pages[index];
        const key = sectionKey(p);
        if (!p || !p.level || !key) return index;
        const remembered = s.sectionMemory[key];
        if (remembered === undefined) return index;
        const rp = s.pages[remembered];
        return (rp && sectionKey(rp) === key) ? remembered : index;
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
        /* The Movy layout turns like Movy — see movy_knob.mjs — not like
         * Schwung's own dial/bar grid (knob_engine.mjs, a different,
         * time-based acceleration feel that predates this port). Same state
         * slot, different init/tick pair, picked once per key so a turn
         * mid-gesture never switches models under your hand. */
        const useMovy = s.layout === LAYOUT_MOVY;
        let st = s.knobStates[key];
        if (!st) {
            const current = s.values[key] !== undefined ? Number(s.values[key]) : Number(getParam(fullKey(key)));
            const start = isFinite(current) ? current : 0;
            st = s.knobStates[key] = useMovy ? movyKnobInit(start) : knobInit(start);
        }

        let value;
        if (useMovy) {
            value = movyKnobTick(st, meta, direction, t, fine);
        } else {
            /* Fine adjust: Elektron's [FUNC]+encoder. Holding shift already
             * reveals every value, so precision mode and "show me the
             * numbers" are the same gesture — which is what you want when
             * you are chasing a value.
             *
             * Only floats have a finer step to give. An int already moves in
             * whole units and an enum in whole options; there is nothing
             * below that, and pretending otherwise would just make them feel
             * broken under shift. */
            const cfg = knobConfigFromMeta(meta);
            const canRefine = fine && meta.type === "float";
            value = knobTick(st, canRefine ? { ...cfg, step: (cfg.step || 0.01) / 10 } : cfg, direction, t);
        }
        const wire = formatParamForSet(value, meta);

        s.values[key] = wire;
        s.settleUntil[key] = s.tickCount + SETTLE_TICKS;
        /* Throttled — see SETPARAM_THROTTLE_MS. A miss is never lost: it is
         * left in pendingWrite for tick() to flush once the window passes,
         * and onKnobTouch(false) flushes immediately on release. */
        const lastWrite = s.lastWriteMs[key] || 0;
        if (t - lastWrite >= SETPARAM_THROTTLE_MS) {
            s.lastWriteMs[key] = t;
            delete s.pendingWrite[key];
            setParam(fullKey(key), wire);
        } else {
            s.pendingWrite[key] = wire;
        }
        /* Throttled — see ANNOUNCE_THROTTLE_MS. A continuous fast turn still
         * announces regularly, just not once per raw MIDI detent. */
        const lastAnnounce = s.lastAnnounceMs[key] || 0;
        if (t - lastAnnounce >= ANNOUNCE_THROTTLE_MS) {
            s.lastAnnounceMs[key] = t;
            announce(announceTurn(meta, wire));
        }
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
            /* Release flushes immediately rather than waiting out
             * SETPARAM_THROTTLE_MS — the hand has stopped, so there is no
             * more flooding to protect against, and the settled value should
             * land on the device the instant you let go, not up to 20ms
             * later. */
            const key = keyAt(slot);
            if (key && s.pendingWrite[key] !== undefined) {
                setParam(fullKey(key), s.pendingWrite[key]);
                s.lastWriteMs[key] = now();
                delete s.pendingWrite[key];
            }
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

    /**
     * Re-read the live value of up to MOD_FAST_READS_PER_TICK modulated keys.
     *
     * `values` stays the BASE — what the user dialled in and what a turn edits
     * — and these are the effective values a source is currently driving the
     * param to, drawn as a dot on the arc. Keeping them apart is the whole
     * point: with the pointer chasing an LFO you cannot see what you set.
     *
     * Skips a key that is being turned, for the same reason the value cursor
     * does (`settleUntil`): a read issued before the turn lands after it.
     */
    function refreshModulatedValues(p) {
        const modKeys = [];
        for (const k of p.keys) {
            if (k && s.modCache[k]) modKeys.push(k);
        }
        if (!modKeys.length) {
            /* Nothing modulated: drop stale dots rather than leave them frozen
             * on the arc after a routing is removed. */
            if (s.modCursor !== 0) s.modCursor = 0;
            for (const k in s.modValues) delete s.modValues[k];
            return;
        }
        const n = Math.min(MOD_FAST_READS_PER_TICK, modKeys.length);
        for (let i = 0; i < n; i++) {
            const key = modKeys[(s.modCursor + i) % modKeys.length];
            /* Deliberately NOT gated on settleUntil, unlike the value cursor.
             * That gate exists because a stale read of the BASE lands after a
             * turn and drags the knob backwards — a write-back race. There is
             * no such race here: the UI never writes the effective value, it
             * only displays it. Gating it meant the dot froze for the whole
             * time you were turning the knob, which is exactly when you most
             * want to see where modulation is putting the param. */
            const v = getParam(fullKey(key));
            if (v !== null && v !== undefined) s.modValues[key] = v;
        }
        s.modCursor = (s.modCursor + n) % modKeys.length;
        /* A key that stopped being modulated keeps no dot. */
        for (const k in s.modValues) {
            if (!s.modCache[k]) delete s.modValues[k];
        }
    }

    function setLayout(layout) { s.layout = layout; }
    function setReveal(on) { s.revealValues = !!on; }
    function setDecorations(d) { s.decorations = d || null; }

    /* Movy layout is a whole separate renderer (its own fixed-geometry header
     * and knob grid, not a `layout` value render_page.mjs understands — see
     * render_page_movy.mjs), so it does not take decorations (a sequencer's
     * per-slot p-locks) or an embedding `rect`: it draws its own header full
     * width, the way Movy itself always does. Anything using those keeps
     * LAYOUT_DIAL/LAYOUT_BAR — see setLayout. */
    function render(ctx, { title, rect } = {}) {
        if (s.layout === LAYOUT_MOVY) {
            const drawGrid = () => renderPageMovy(ctx, {
                page: page(), metaIndex: s.metaIndex, values: s.values,
                title: title || "", pageIndex: s.pageIndex, pageCount: s.pages.length,
                touched: s.hintLines ? -1 : s.touched,
                modulated: (key) => !!s.modCache[key],
                modValues: s.modValues,
                pageGroups: pageGroups(),
                viz: vizEnabled ? vizGroups() : [],
            });
            if (s.hintLines) {
                drawGrid();
                renderHint(ctx, { rect, lines: s.hintLines.lines, title: s.hintLines.title });
                return;
            }
            if (s.pickerOpen) {
                renderPicker(ctx, { rect, entries: s.pickerEntries, index: s.pickerIndex, title: "Sections" });
                return;
            }
            drawGrid();
            return;
        }

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
            modulated: (key) => !!s.modCache[key],
            /* Section ids for the page rule, so it groups the way Shift+jog
             * navigates. Cached — it only changes when the page set does. */
            pageGroups: pageGroups(),
            /* A sequencer's parameter-lock decorations are per SLOT; a
             * graphic replacing several slots with one picture would hide
             * which of them is locked, so graphics stand down while
             * decorations are active. */
            viz: (vizEnabled && !s.decorations) ? vizGroups() : [],
        });
    }

    let vizCache = null;
    function vizGroups() {
        const p = page();
        if (!p || p.kind !== PAGE_KNOBS || !s.metaIndex) return [];
        const cacheKey = `${s.fingerprint}#${s.pageIndex}`;
        if (vizCache && vizCache.key === cacheKey) return vizCache.groups;
        const { groups } = resolveViz({ keys: p.keys, metaIndex: s.metaIndex, overrides: vizOverrides });
        vizCache = { key: cacheKey, groups };
        return groups;
    }

    /** Read the current page aloud — the gesture that replaces a glance. */
    function announceContents() {
        announce(announcePageContents(page(), s.metaIndex, s.values, s.decorations));
    }

    let groupCache = null;
    function pageGroups() {
        if (groupCache && groupCache.fp === s.fingerprint) return groupCache.groups;
        const groups = s.pages.map((p) => (p.level === null || p.level === undefined) ? p.kind : p.level);
        groupCache = { fp: s.fingerprint, groups };
        return groups;
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
        /** This key's modulation flag as of the last time the cursor reached
         *  it. Read-only view of the cache the renderer uses — the injected
         *  isModulated is deliberately NOT called during a draw. */
        isModulatedCached: (key) => !!s.modCache[key],
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
