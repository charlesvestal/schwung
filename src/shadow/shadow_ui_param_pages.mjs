/*
 * Shadow UI — Param Pages (the knob-grid parameter view).
 *
 * A preview alternative to the hierarchy list editor: a module's declared
 * parameters laid out eight to a page across the physical knobs, instead of a
 * scrolling list. Off by default — Global Settings -> Display -> Param View.
 *
 * Almost nothing lives here. The page model, metadata resolution, rendering,
 * navigation, screen-reader strings, the whole interaction model and the MIDI
 * decoding are in `shared/param_pages/`, pure and tested headlessly against a
 * fake device (tools/param-pages/, tests/host/test_param_pages_*.sh). What is
 * left in this file is the part that genuinely needs the shadow UI: which slot
 * and component we are pointed at, the per-frame tick, and handing off the
 * screens the controller deliberately refuses to own.
 *
 * Two hand-offs, both deliberate:
 *   - an opaque param (filepath / canvas / wav_position / string) returns an
 *     "open" intent and the LIST editor's existing screen handles it. The grid
 *     never reimplements a file browser.
 *   - a non-grid page kind (preset browser, items list, mode select, child
 *     selector) is drawn by the screens that already exist. The grid draws
 *     grids.
 *
 * State accessors come from the shared `ctx` (populated by shadow_ui.js); see
 * shadow_ui_ctx.mjs. As with the other view modules, only touch ctx.* inside
 * function bodies, never at top level.
 */

import { ctx } from './shadow_ui_ctx.mjs';
import { createController, CONTRACT_SETTLE_MS, LAYOUT_LIST } from '/data/UserData/schwung/shared/param_pages/page_controller.mjs';
/* Re-exported so a contract can PIN its layout in the chrome it already hands
 * over (see paramPagesLayout). Global Settings does; slot and Master FX
 * settings deliberately do not. */
export { LAYOUT_LIST };
/* Re-exported so the LIST editor waits out the same module-side debounce the
 * grid does, from the same number. Two hand-written 500s would drift. */
export { CONTRACT_SETTLE_MS };
import { decodeInput, applyInput } from '/data/UserData/schwung/shared/param_pages/page_input.mjs';
import { PAGE_KNOBS, PAGE_MENU, PAGE_PRESET, PAGE_ITEMS } from '/data/UserData/schwung/shared/param_pages/page_plan.mjs';
import { LAYOUT_MOVY } from '/data/UserData/schwung/shared/param_pages/render_page_movy.mjs';
import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';
import { log, isLoggingEnabled } from '/data/UserData/schwung/shared/logger.mjs';

/* The live controller, or null when the view is not open. One at a time: the
 * grid always shows a single component, and rebuilding on entry is cheap. */
let controller = null;
/* Which param accessors the live controller closes over, so switching between
 * a module and a synthesised contract (slot settings) rebuilds it instead of
 * silently keeping the old ones. */
let controllerIo = null;
let currentSlot = 0;
let currentComponent = 'synth';
/* The component's DSP param prefix, which is NOT the component key: the MIDI
 * FX component is `midiFx` and its params live under `midi_fx1` (see
 * getComponentParamPrefix in shadow_ui.js). Anything asking the device for a
 * param has to use this — `midiFx_module` and `midiFx:is_loading` are keys no
 * one serves, so the header abbreviation read "--" and is_loading never fired. */
let currentPrefix = 'synth';
/*
 * CHROME — the three things that differ between the two chain editors, handed
 * in as DATA rather than worked out here.
 *
 * The grid is opened from a slot chain and from Master FX, and those two
 * spell three things differently:
 *
 *   label      the header band says "S2" for a slot chain and "MFX" for the
 *              master bus -- which is addressed at IPC slot 0 by convention
 *              and is NOT instrument slot 1, so `S${slot+1}` would be a lie.
 *   moduleKey  the key naming the module behind the view. A slot chain says
 *              "fx1_module"; Master FX says "master_fx:fx1:module". Get this
 *              wrong and the read is merely unserved -- an unserved key reads
 *              back as "" -- so the header quietly loses its name instead of
 *              failing.
 *   returnView where Back goes. Hardcoding VIEWS.CHAIN_EDIT here dropped a
 *              Master FX user into the slot chain editor.
 *
 * Deliberately NOT a `startsWith("master_fx:")` test in this file. One place
 * knows there are two chain editors (chainEditorFocus / the chain targets in
 * shadow_ui.js); a second copy of that knowledge here is exactly the drift
 * that left Master FX without the knob card for a day.
 *
 * null means the slot-chain defaults, so the four existing call sites are
 * unchanged.
 */
let currentChrome = null;

/**
 * Shift state does NOT arrive as MIDI here.
 *
 * The shim forwards a deliberately short list to the shadow UI — CC 3, 14, 51,
 * 40-43, 71-78, 88, plus notes 0-7, 40-43 and (when pad_block is set) 68-99.
 * CC 49 is not on it: the shim tracks shift itself and publishes it in shared
 * memory, which is why the rest of shadow_ui.js reads shadow_get_shift_held()
 * rather than watching for a CC.
 *
 * Getting this wrong is silent — every shift gesture (section step, reveal
 * values, fine adjust, reset to default) simply never fires, with nothing in
 * the logs. An overtake TOOL sharing this library does receive CC 49, which is
 * why page_input.mjs still decodes it; only this host reads it out of band.
 */
function shiftIsHeld() {
    return typeof shadow_get_shift_held === 'function' && shadow_get_shift_held() !== 0;
}

/** Param View setting values. */
export const PARAM_VIEW_LIST = 0;
export const PARAM_VIEW_KNOBS = 1;

/**
 * Whether the page chrome may run at all.
 *
 * ONE question, and it is no longer "is the grid on". `param_view` used to be
 * answered here too, which made choosing List fork the user into an entirely
 * separate engine — the hierarchy editor in shadow_ui.js, ~34 functions and
 * ~506 references. Now that PAGE_KNOBS has a list LAYOUT (LAYOUT_LIST), List is
 * an arrangement inside this engine, so it is paramPagesLayout()'s to answer
 * and this function no longer looks at it.
 *
 * What remains is the screen reader, and it is UNCHANGED on purpose. A grid has
 * eight cells and nothing selected, and the controller's list layout has
 * announcements nobody has validated on hardware; until they are, TTS keeps
 * reaching the hierarchy editor exactly as before. Flipping that is a single
 * deliberate act with the whole fleet behind it (design §6), not a side effect
 * of splitting this seam.
 */
export function paramPagesEnabled() {
    if (typeof tts_get_enabled === 'function' && tts_get_enabled()) return false;
    return true;
}

/**
 * WHICH arrangement the page chrome draws — the other half of the split above.
 *
 * The two layouts share everything a page is (page_plan, param_meta,
 * knob_engine, param_format, announce_page, the chrome in render_page_movy);
 * they differ only in pixel arrangement, which is the one difference the design
 * calls irreducible. So this is a value handed to the controller AT THE DRAW
 * CALL SITE, never a flag threaded down into widget code — that is the `geom`
 * all-or-nothing trap in another costume.
 *
 * THE SCREEN READER FORCES THE LIST, and that rule lives here rather than at a
 * call site. paramPagesEnabled() above keeps TTS users on the hierarchy editor
 * for every COMPONENT, so this looks redundant — but Global Settings is a
 * contract with no other path at all (its bespoke list is gone), and it is the
 * screen you go to in order to turn the screen reader OFF. Reaching it with TTS
 * on and getting eight cells with nothing selected would be the one place a
 * blind user cannot get back out of. A grid announces a page; a list announces
 * a row.
 */
/*
 * A contract may PIN its layout, and Global Settings does.
 *
 * Param View is a preference about module parameters — cutoff, resonance,
 * decay — where eight cells you can grab at once is the whole point. Global
 * Settings is not that. Every one of its 25 params is a set-once toggle, and
 * several are destructive to brush past: link_audio_routing re-routes Move's
 * audio, resample_bridge replaces the sampler's input, param_view changes the
 * screen you are standing on. A knob has no detent to tell you that you have
 * changed something.
 *
 * So the pin is a property of the CONTRACT, not of the user's preference, and
 * it lives in the chrome the contract already hands over rather than in a
 * `component === "global_settings"` test here — this function must not learn
 * the names of screens. Slot Settings and Master FX Settings deliberately do
 * NOT pin: their Volume, Mute and Solo genuinely are performance controls.
 */
export function paramPagesLayout() {
    if (currentChrome && currentChrome.layout) return currentChrome.layout;
    if (typeof tts_get_enabled === 'function' && tts_get_enabled()) return LAYOUT_LIST;
    const mode = typeof param_view_get_mode === 'function' ? param_view_get_mode() : PARAM_VIEW_LIST;
    return mode === PARAM_VIEW_KNOBS ? LAYOUT_MOVY : LAYOUT_LIST;
}

/**
 * Point the grid at a component. Safe to call on every entry — the controller
 * rebuilds only when the declared contract actually changed.
 *
 * @param {number} slot        chain slot 0-3
 * @param {string} component   'synth' | 'fx1' | 'fx2' | 'midiFx' | 'master_fx:fx1' …
 * @param {string} prefix      the DSP param prefix for that component
 */
/* No first-use overlay. The grid used to open behind a panel listing its
 * gestures, because they are not guessable and a preview nobody can operate
 * produces no useful feedback. The hint FOOTER carries that now — it names the
 * jog, the click and one contextual gesture on every page, permanently, in
 * eight rows bought from the label bands — so the panel taught something the
 * screen already says and cost a modal on entry to do it.
 *
 * The library's showHint/renderHint stay: they are caller-supplied, and a tool
 * embedding the grid with its own gestures may still want one. This host does
 * not. */

/**
 * @param {string} [restorePageName]  land on the page with this name instead of
 *   the first one. Used when an editor hands control back: the user was on
 *   page 5 when they clicked a sample, and page 1 is not where they were.
 *   Matched by NAME, not index — controller.load rebuilds the page set and
 *   every index can shift (same reason page_nav reanchors by name).
 * @param {object} [io]  {getParam,setParam} to use instead of the slot/component
 *   default. Slot settings needs it: a slot publishes no ui_hierarchy and its
 *   params do not share one prefix, so the contract and the mapping are handed
 *   in (see shadow_ui_slot_grid.mjs) rather than read off a component.
 * @param {object} [chrome]  {label, name, moduleKey, returnView, onExit} — see
 *   currentChrome. onExit replaces returnView for a contract with no view above
 *   it (Global Settings); it is called instead of setView on Back.
 *   Omitted means the slot-chain defaults.
 */
export function enterParamPages(slot, component, prefix, restorePageName, io, chrome, restoreOpts) {
    currentSlot = slot;
    currentComponent = component;
    currentPrefix = prefix || component;
    /* Reset, not merge: an entry that supplies no chrome IS the slot chain, and
     * carrying the last one over would leave a Master FX header on it. */
    currentChrome = chrome || null;

    /* Rebuild when the accessors change, not just when there is no controller:
     * it CLOSES OVER them, so one built for a module would keep reading the
     * module after a switch to slot settings. */
    if (!controller || controllerIo !== (io || null)) {
        controllerIo = io || null;
        /*
         * The DEFAULTS, then the caller's io spread over the top.
         *
         * Spread, not field-by-field. Picking the fields by hand dropped
         * `formatValue` on the floor: the slot-settings contract supplied one,
         * the controller expected one, and this line — the only thing between
         * them — did not mention it. Both ends were tested; the join was not,
         * and an LFO target went on reading "FX1" on the device while every
         * test passed. Spreading makes a new capability arrive by default, so
         * the failure mode is at worst an ignored key rather than a silently
         * missing one.
         *
         * `announce` is deliberately not overridable this way today; nothing
         * supplies one, and it would be spread over if something did.
         */
        controller = createController(Object.assign({
            getParam: (key) => ctx.getSlotParam(currentSlot, key),
            setParam: (key, value) => ctx.setSlotParam(currentSlot, key, value),
            announce,
            /* The list editor marks these with "~"; the grid ticks the cell.
             * A synthesised contract may answer for itself — slot settings
             * does, because the generic oracle both got it wrong for `slot:*`
             * keys and cost three IPC round trips per tick to do so. */
            isModulated: (key) => (typeof ctx.isParamModulated === 'function'
                ? !!ctx.isParamModulated(currentSlot, key) : false),
        }, io || {}));
    }
    /* Entering the view is the only way the module behind it can have changed,
     * so this is where the cached abbreviation is dropped. */
    _abbrevCache = null;
    /* New module behind the view — it may well implement is_loading even if
     * the last one didn't, so start asking at full rate again. */
    _loadingInterval = LOADING_POLL_TICKS;
    _loadingPoll = 0;
    /* `visible` resolves visible_if conditions. The default binds to the LIST
     * editor slot/component, which is stale while the grid is up — fine for a
     * component (the grid and the list agree on which one), wrong for a
     * synthesised contract, so an io may carry its own. */
    controller.load({
        slot, component, prefix: prefix || component,
        visible: (io && io.visible) ? io.visible : ctx.evaluateVisibilityCondition,
    });
    /* "Knobs" IS schwung-movy's own knob-page layout now, not Schwung's
     * earlier dial/bar grid — see render_page_movy.mjs. "List" is the same
     * engine with the knob page arranged as five rows (LAYOUT_LIST). The
     * setting stays a plain List/Knobs toggle; this is which one it draws.
     *
     * Set here as well as on the draw path because INPUT can arrive before the
     * first frame does, and the controller's jog/click model reads its layout
     * (a list has a row cursor; a grid does not). The draw path is what keeps
     * it live if the setting changes while the view is up. */
    controller.setLayout(paramPagesLayout());
    /*
     * Hand the NAME to the controller rather than resolving it here.
     *
     * This used to look through controller.pages once and give up if the page
     * was not there. Coming back from granny's file browser it is not: granny
     * loads the WAV synchronously inside set_param, on the SPI thread that
     * also serves param reads, so the contract read straight after a sample
     * selection times out and the planner correctly refuses to invent pages
     * from a failed read. The list was empty, the loop found nothing, and you
     * landed on page 1 -- reported from the device.
     *
     * controller.restorePage() re-applies the request each time the pages are
     * planned, and drops it once the contract settles without producing that
     * page.
     */
    /* restoreOpts.enter decides whether the restored page's door OPENS --
     * see restorePage. Only the caller knows whether we are coming back from
     * finishing something (jog back to paging) or from merely looking (stay
     * inside the menu you never really left). */
    if (restorePageName) controller.restorePage(restorePageName, restoreOpts || {});
    ctx.setView(ctx.VIEWS.PARAM_PAGES);
}

export function exitParamPages() {
    controller = null;
    controllerIo = null;
}

/*
 * Rebuild ONLY the trailing pages ("My Presets" / "Module"), in place —
 * after a Save or Delete changes what the My Presets rows offer, so the
 * grid reflects it without moving the user off the page they are standing
 * on. No-op when the grid is not up (e.g. a save committed from the
 * module-picker's own preset browser, which never opened the grid).
 */
/* Close the menu on the page that is up, without leaving the page. Save acts
 * in place -- it never navigates -- so it has no return path to carry the
 * "you are finished here" disposition. This is that disposition. */
export function paramPagesExitMenu() {
    if (controller && typeof controller.exitMenu === 'function') controller.exitMenu();
}
export function paramPagesRefreshTrailing() {
    if (controller) controller.refreshTrailing();
}

export function paramPagesActive() {
    return controller !== null;
}

/** Which component the grid is pointed at, for handing back to the list. */
export function paramPagesComponent() {
    return currentComponent;
}

/** Which slot the grid is pointed at, for handing back to the list. */
export function paramPagesSlot() {
    return currentSlot;
}

/** The page the grid is on, so the host can decide whether it draws it. */
export function currentParamPage() {
    return controller ? controller.page : null;
}

/**
 * Once per frame. Polls for a contract that changed underneath us (a module
 * finishing an async ROM or sample load republishes a larger tree) and advances
 * the staggered read cursor by exactly one param.
 */
export function tickParamPages() {
    if (!controller) return;

    /* Only re-plan on the loading->ready edge; re-planning every frame would
     * reset values and the cursor continuously.
     *
     * Polled on a divider, not every tick. Every one of these is a synchronous
     * round trip (~2.8ms, serviced once per SPI frame) and on device this was
     * 1.0 of the grid's 7.1 reads per tick — for an edge that fires once, when
     * a module finishes loading. Checking it ~8x less often delays the re-plan
     * by at most LOADING_POLL_TICKS, which is invisible next to the module
     * load it is waiting on. */
    if (++_loadingPoll >= _loadingInterval) {
        _loadingPoll = 0;
        const raw = ctx.getSlotParam(currentSlot, `${currentPrefix}:is_loading`);
        if (raw === '') {
            /*
             * NOBODY SERVES THIS KEY. Stop asking, for this component.
             *
             * An unserved key answers "" — the shim replies with an error and
             * a zeroed buffer, and the binding hands that back as an empty
             * string. Only an unclaimable channel answers null. The back-off
             * below tested for null, so for the overwhelming majority of the
             * fleet — which does not implement is_loading at all — it never
             * engaged: the poll fell through to the `else`, read "" as
             * not-loading, reset the interval to full rate, and did that
             * forever. Measured on device at 5-7 errored reads a SECOND, and
             * an errored read still costs the whole ~2.8 ms round trip.
             *
             * Giving up entirely is safe now in a way it was not before,
             * because the contract no longer depends on this edge: a selection
             * books its own settle in the controller (armContractSettle), and
             * that path needs no cooperation from the module.
             */
            _loadingInterval = Infinity;
        } else if (raw === null || raw === undefined) {
            /* The channel would not answer — transient, unlike "". Back off
             * rather than give up: a module genuinely mid-load can fail a
             * claim first and answer later, and this is what re-plans the page
             * tree when it finishes. Reset on entry and on any real read. */
            if (_loadingInterval < LOADING_POLL_MAX_TICKS) _loadingInterval *= 2;
        } else {
            _loadingInterval = LOADING_POLL_TICKS;
            const loading = raw === '1';
            if (!loading && wasLoading) controller.reloadIfChanged({ visible: ctx.evaluateVisibilityCondition });
            wasLoading = loading;
        }
    }

    /* The grid paces its own redraws (MOVY_REDRAW_MIN_MS), so it does not want
     * the global every-other-tick gate on top: measured, that held it to 0.34
     * draws per tick — ~20fps against a 42/s tick — because a knob turn does
     * not set `needsRedraw`. Asking every tick hands the pacing decision to
     * the grid, where the measurement lives. */
    _tickCount++;
    if (typeof ctx.requestRedraw === 'function') ctx.requestRedraw();

    /* Shift is polled, not evented (see shiftIsHeld), so reveal follows it here
     * rather than on a CC that never arrives. */
    controller.setReveal(shiftIsHeld());

    controller.tick();
}
let wasLoading = false;
/* is_loading is an edge that fires once per module load; polling it every tick
 * cost a full IPC round trip per frame. See tickParamPages. */
const LOADING_POLL_TICKS = 8;
/* Ceiling for the backoff when the module does not implement the key at all
 * (~2.7s at 60Hz) — rare enough to be free, frequent enough that a late-
 * appearing answer is still noticed. */
const LOADING_POLL_MAX_TICKS = 160;
let _loadingPoll = 0;
let _loadingInterval = LOADING_POLL_TICKS;
/* Module id per (slot, component), read once instead of on every draw — it
 * changes only on a module swap, which goes through openParamPages. */
let _abbrevCache = null;

/**
 * Minimum gap between full redraws of the grid. ZERO — the throttle is off.
 *
 * It used to be 32ms, on the reasoning that a fast turn demands the most
 * frequent redraws at exactly the moment each one is most expensive ("a live
 * curve recomputing every tick, real per-pixel geometry"). Every part of that
 * has since been measured and none of it holds:
 *
 *   a whole page render          1.62ms   (src/shared/draw_bench.mjs)
 *   js.tick p50                  311us    (OTLP, after the read fixes)
 *   host tick rate               42.3/s
 *   grid draw rate WITH the 32ms throttle    ~18fps
 *
 * Drawing every single tick costs 42 x 1.62ms = 68ms per second, under 7% of
 * one core. The throttle was not protecting anything; it was the binding
 * constraint on the whole view. Worse than 32ms in practice: this device's
 * clock is quantised to ~11-12ms, so the comparison rounds up to a 33-44ms
 * gate, and tick phase jitter drops it to ~18fps — the screen updating 18
 * times a second while the hardware offers 42. That is the "laggy knobs"
 * report, and no amount of IPC reduction moves it.
 *
 * The original "fast turns feel worse" symptom was real, but its cause was
 * setParam being called once per raw detent — fixed by SETPARAM_THROTTLE_MS
 * in page_controller.mjs, as the note there says. This was belt-and-braces on
 * top of a fix that had already landed.
 *
 * Stays zero. The tick itself is now paced to an absolute deadline
 * (shadow_ui.c), so it arrives at a steady 60 Hz regardless of how much work
 * a tick does — which is what the irregular frame rate actually was. Gating
 * the draw on top of a steady tick would only throw frames away.
 *
 * Raise it only with a measurement, not a hypothesis. That is how it came to
 * be 32 in the first place, guarding against a draw cost (1.68ms/page) that
 * was never the problem, and it then became the binding constraint on the
 * whole view.
 */
const MOVY_REDRAW_MIN_MS = 0;
let lastDrawMs = 0;

/**
 * Draws-per-second, logged once a second while the grid is on screen
 * (nothing unless `debug_log_on` is set — see docs/LOGGING.md).
 *
 * Deliberately a COUNT over a ~1s window rather than a Date.now() duration:
 * this device's clock is quantized to roughly 11-12ms (proven by 20
 * back-to-back Date.now() calls with no work between them returning the
 * identical value), which makes any single render's measured "duration"
 * meaningless — it is rounding to the next tick, not timing real work. A
 * count averages that quantization out over enough ticks to mean something,
 * and it is what actually diagnosed the "fast turns feel like lower fps"
 * report: it fell from ~17 to 5-9 specifically under a MIDI flood (a fast
 * physical spin decodes to 250-320 CC messages/second), which traced to
 * setParam being called once per raw detent — see SETPARAM_THROTTLE_MS in
 * page_controller.mjs, the actual fix. Kept as a standing diagnostic for the
 * open on-device question in docs/plans/2026-08-16-next-sessions.md
 * "Session C" (redraw/IPC timing was never verified on hardware). */
let _fpsWindowStart = 0, _fpsCount = 0;
/* Counted in tickParamPages, reported with the draw count above. */
let _tickCount = 0;

/** Draw. Non-grid pages are not ours — the host dispatches those. */
/*
 * Span helper for the two things inside a grid tick that the trace could not
 * see: the draw itself, and MIDI handling. `js.tick` and `param.get/set` were
 * instrumented, so IPC was attributable and everything else was one
 * undifferentiated lump — which is exactly where the remaining cost turned
 * out to live once the IPC was cut. No-ops unless otlp_trace_on is present
 * (host_trace_begin returns 0 and end ignores it). Pairs must balance inside
 * one tick; the finally does that even if the body throws.
 */
function traced(name, fn) {
    const h = (typeof host_trace_begin === 'function') ? host_trace_begin(name) : 0;
    try { return fn(); }
    finally { if (h && typeof host_trace_end === 'function') host_trace_end(h); }
}

/*
 * What the footer says, per context.
 *
 * Ordered MOST IMPORTANT FIRST, because drawFooter drops the tail rather than
 * squeezing it: three pairs only fit when every word is <= 4 characters, and
 * two always fit. So Back leads wherever losing it would strand you.
 *
 * Kept here, not in the library: these are Schwung's gestures. A sequencer
 * embedding the same grid has its own and passes its own.
 */
/*
 * Does Shift+Jog actually differ from Jog on THIS module?
 *
 * stepLevel skips pages belonging to the level you are already on, so it only
 * differs where a level spans more than one page. granny has six pages and six
 * distinct levels, so Shift+Jog walks exactly the same sequence as Jog — and a
 * footer saying JOG SECT there is advertising a distinction the module does not
 * have. minijv, at 76 pages over ~20 levels, is where it earns its place.
 *
 * Memoised on the pages array itself, which is replaced whenever the controller
 * rebuilds, so a module swap recomputes without any explicit invalidation.
 */
let _sectionsPages = null;
let _sectionsDiffer = false;
function sectionsAreDistinct() {
    const pages = controller ? controller.pages : null;
    if (!pages || !pages.length) return false;
    if (pages === _sectionsPages) return _sectionsDiffer;
    _sectionsPages = pages;
    const seen = new Set();
    _sectionsDiffer = false;
    for (const p of pages) {
        const lv = p && p.level;
        if (lv == null) continue;
        if (seen.has(lv)) { _sectionsDiffer = true; break; }
        seen.add(lv);
    }
    return _sectionsDiffer;
}

/*
 * Build the footer in a FIXED ORDER: jog first, click second, anything else
 * after. Positional, not descriptive — the eye learns that slot 1 is the wheel
 * and slot 2 is the button and stops re-reading them. When a state has no jog
 * or no click meaning, the slot is simply absent; nothing else slides into it.
 *
 * Enforced here rather than at each call site, because "remember to put jog
 * first" is exactly the kind of rule that holds for three states and breaks on
 * the fourth — which is what happened: the held-knob footer led with CLK and
 * the others led with JOG, so the two pills swapped places under your finger.
 */
function orderedHints({ jog, click, extra }) {
    const out = [];
    if (jog) out.push(["JOG", jog]);
    if (click) out.push(["CLK", click]);
    for (const e of (extra || [])) if (e) out.push(e);
    return out;
}

function footerHints() {
    if (!controller) return null;

    /* "EXIT", not "CLOSE": with CLOSE the three pairs are 129px — one pixel
     * over — and the pair dropped was CLK GO, i.e. how you commit. */
    if (controller.pickerOpen) {
        return orderedHints({ jog: "SECT", click: "GO", extra: [["BACK", "EXIT"]] });
    }

    const shift = shiftIsHeld();
    const fine = shift ? [["KNB", "FINE"]] : null;

    /*
     * A menu page is a door at page scale: inert until entered, so the jog
     * still pages and the click is what goes in. Once inside, the jog drives
     * the list and Back comes out — the same ladder a picker has.
     */
    const mp = controller.page;
    if (mp && mp.kind === "menu") {
        return controller.menuEntered && controller.menuEntered()
            ? orderedHints({ jog: "SEL", click: "OPEN", extra: [["BACK", "OUT"]] })
            : orderedHints({ jog: "PAGE", click: "ENTER", extra: fine });
    }

    /*
     * A preset browser is the same door, and the footer is where the promise
     * lives: OUTSIDE it the jog pages, so scrolling past a synth cannot load
     * its presets; INSIDE it the jog is the browser and every step auditions.
     * Saying which of the two you are in is the entire safety of the thing.
     */
    /* A runtime item list: scrolling it writes nothing, so unlike the preset
     * browser there is no auditioning to warn about — CLK LOAD is the whole
     * story, and it is only true once you are inside. */
    if (mp && mp.kind === PAGE_ITEMS) {
        return controller.menuEntered && controller.menuEntered()
            ? orderedHints({ jog: "SEL", click: "LOAD", extra: [["BACK", "OUT"]] })
            : orderedHints({ jog: "PAGE", click: "ENTER", extra: fine });
    }

    if (mp && mp.kind === PAGE_PRESET) {
        /* Three pairs fit only when every word is <= 4 characters, and these
         * are: JOG PRST / CLK EDIT / BACK OUT is 126px. */
        return controller.menuEntered && controller.menuEntered()
            ? orderedHints({ jog: "PRST", click: "EDIT", extra: [["BACK", "OUT"]] })
            : orderedHints({ jog: "PAGE", click: "ENTER", extra: fine });
    }

    /*
     * A knob under the hand changes what the CLICK means and nothing else, so
     * only that slot changes. Holding a knob whose param opens an editor, the
     * click opens it; holding any other knob, the click still opens the section
     * menu, so the footer must keep saying MENU.
     *
     * Keyed on meta.divable, NOT kind === "opaque". Those came apart when a
     * ranged wav_position became a turnable number that still opens a waveform
     * editor, and this line was missed — so holding granny's Position (divable,
     * kind "number") advertised CLK MENU while the click opened the editor. The
     * footer promised one thing and the button did another.
     */
    const held = controller.state ? controller.state.touched : -1;
    if (held >= 0) {
        const meta = controller.metaAt ? controller.metaAt(held) : null;
        /*
         * A TRIGGER is a button: the click does the thing rather than opening
         * anything. Without this it fell through to CLK MENU -- the footer
         * advertised the section menu while the click ran a destructive
         * action, which is the same promise-versus-behaviour mismatch the
         * divable line above exists to prevent.
         *
         * PUSH, not FIRE. The widget is drawn as a push button, and the hint
         * vocabulary should name the GESTURE the picture is asking for, the
         * way JOG SEL and CLK OPEN do. FIRE names the consequence instead,
         * which is a second thing to learn about a control that is already
         * self-explanatory once it looks like a button.
         */
        if (meta && meta.writeOnly) {
            return orderedHints({ jog: "PAGE", click: "PUSH", extra: fine });
        }
        if (meta && meta.divable) {
            return orderedHints({ jog: "PAGE", click: "OPEN", extra: fine });
        }
    }

    /*
     * Shift changes what the JOG does — but only on a module where it actually
     * does something different. stepLevel skips pages of the level you are
     * already on, so on granny (six pages, six distinct levels) Shift+Jog walks
     * exactly the same sequence as Jog. Claiming SECT there advertises a
     * distinction the module has not got; 18 of the 72 fleet modules are like
     * that. minijv, 76 pages over ~20 levels, is where it earns its place.
     */
    const jog = (shift && sectionsAreDistinct()) ? "SECT" : "PAGE";
    return orderedHints({ jog, click: "MENU", extra: fine });
}

/**
 * The header band, "<chain> > <name>".
 *
 * Exported for the same reason paramPagesFooterHints() is: the movy renderer
 * sets this in its own font, drawing every glyph as fillRect pixels, so a
 * recording print() sees nothing at all and the string cannot be read back off
 * the framebuffer. It is built HERE and used by the one draw call, so what is
 * asserted is what is drawn rather than a second copy of the rule.
 */
export function headerTitle() {
    /* Cached: this was a synchronous round trip on EVERY draw (1.4 of the
     * grid's 7.1 reads per tick, measured on device) to render a two-letter
     * abbreviation that cannot change without going back through
     * openParamPages, which clears the cache. */
    /* Slot settings is a synthesised contract, not a module — there is no
     * "slot_module" to abbreviate, so the lookup returned nothing and the
     * header read "S1 > ---". It has a name of its own. */
    if (currentComponent === 'slot') _abbrevCache = 'Settings';
    /* Any other synthesised contract says its own name through the chrome —
     * Master FX settings is one, and there is no "master_settings_module" to
     * abbreviate either. Declared as DATA rather than as a second literal
     * component name here, so a third one needs no edit to this file. */
    if (currentChrome && currentChrome.name) _abbrevCache = currentChrome.name;
    if (_abbrevCache === null) {
        /* The master bus spells this "master_fx:fx1:module"; a slot chain
         * spells it "fx1_module". An unserved key reads back as "" rather than
         * erroring, so the wrong spelling loses the name silently. */
        const moduleKey = (currentChrome && currentChrome.moduleKey)
            || `${currentPrefix}_module`;
        /* The NAME, not the abbreviation. An abbreviation is a placeholder
         * for a name that has not arrived; on Master FX, where a module often
         * has no presets, it was the permanent answer and the header read
         * "MFX > CS" forever. getModuleDisplayName falls back to the
         * abbreviation until module.json has been read, so nothing blanks. */
        const moduleRef = ctx.getSlotParam(currentSlot, moduleKey) || '';
        _abbrevCache = ctx.getModuleDisplayName
            ? ctx.getModuleDisplayName(moduleRef)
            : (ctx.getModuleAbbrev ? ctx.getModuleAbbrev(moduleRef)
                                   : currentComponent.toUpperCase());
    }
    /*
     * A loaded USER preset takes priority over the module's own patch name --
     * asked for and answered yes on hardware ("should we change the preset in
     * the header from the system preset to the user preset? (Init -> tst)
     * and then show the * there too?"). Marked the SAME way the My Presets
     * page's own row is (`* ` leading, never trailing -- see
     * current_preset.mjs presetRowValue), and read from a cache
     * (userPresetHeaderMark), never the DSP, so this costs nothing beyond the
     * read the My Presets page already pays for.
     *
     * A synthesised contract (Slot Settings, Master FX/Global Settings) or a
     * Master FX component never has a record for its key, so this answers
     * null there and falls through unaffected.
     *
     * A hardware synth puts the PATCH name in its display, not the model
     * number — and the module's identity is already visible in the chain
     * editor you came from. Falls back to the abbreviation until the read
     * cursor has picked the name up, and for modules with no presets. */
    const userMark = (typeof ctx.userPresetHeaderMark === 'function')
        ? ctx.userPresetHeaderMark(currentSlot, currentComponent) : null;
    const name = userMark ? `${userMark.dirty ? '* ' : ''}${userMark.name}`
        : (controller && controller.presetName) || _abbrevCache;
    /* "MFX", never "S1", on the master bus: it is ADDRESSED at IPC slot 0 by
     * convention and is not instrument slot 1. */
    const label = (currentChrome && currentChrome.label) || `S${currentSlot + 1}`;
    return `${label} > ${name}`;
}

export function drawParamPages() {
    if (!controller) return false;
    /* The section picker is drawn over whatever page you were on, including a
     * non-grid one, so it is checked before the page kind. */
    const page = controller.page;
    /*
     * PAGE_MENU and PAGE_PRESET are drawn by the controller in the page chrome,
     * so they must NOT be refused here. Refusing a kind makes the host run its
     * fallback — enterHierarchyEditorFromParamPages — which enters the
     * hierarchy editor for the component. For slot settings that component is
     * "slot", which has no ui_hierarchy, so jogging to the actions page ejected
     * straight to "No presets".
     *
     * PAGE_PRESET joined them because that eject was worse than ugly: the list
     * editor it landed in has the jog wired to the preset browser, so jogging
     * PAST a synth's preset page on the way somewhere else loaded every preset
     * it crossed. It is a door now — inert until you click into it.
     *
     * PAGE_ITEMS joined them too: a soundfont or NAM-model list is a real
     * list, so unlike a preset level it can be five rows in the page chrome
     * rather than a separate screen.
     *
     * The remaining kinds (modes, child) genuinely belong to screens this file
     * does not own, and still hand off.
     */
    const drawable = page && (page.kind === PAGE_KNOBS || page.kind === PAGE_MENU
                              || page.kind === PAGE_PRESET || page.kind === PAGE_ITEMS);
    /*
     * A contract we could not READ has no page set, and refusing to draw ejects
     * to the list editor — a whole view change, in response to a 100 ms timeout
     * that is usually about to clear. So hold the screen while the controller
     * retries (it stops claiming to be unresolved once it gives up, and the
     * ordinary fallback runs then) rather than showing a plan built from the
     * failure, which is what put granny's sample_path on knob 1.
     *
     * Only reachable on a FIRST entry: a re-entry keeps the page set it already
     * had, so the reported granny sequence never shows this.
     */
    const holdForContract = !controller.pickerOpen && !drawable && controller.contractUnresolved;
    if (!controller.pickerOpen && !drawable && !holdForContract) return false;

    const nowMs = Date.now();
    if (nowMs - lastDrawMs < MOVY_REDRAW_MIN_MS) return true;
    lastDrawMs = nowMs;

    _fpsCount++;
    if (!_fpsWindowStart) _fpsWindowStart = nowMs;
    else if (nowMs - _fpsWindowStart >= 1000) {
        /* Ticks alongside draws, because "dropping frames" has two completely
         * different causes and this one line separates them: draws << ticks
         * means something is gating the redraw, draws ~= ticks but both low
         * means the tick itself is too slow (almost always IPC — a read is
         * ~2.8ms against a 1.68ms whole-page render). */
        console.log(`param_pages fps: ${_fpsCount} draws / ${_tickCount} ticks / ${nowMs - _fpsWindowStart}ms`);
        _fpsWindowStart = nowMs;
        _fpsCount = 0;
        _tickCount = 0;
    }

    clear_screen();

    /* The hold frame — see holdForContract above. Deliberately after the
     * redraw throttle, so a screen that says one word costs one draw per
     * MOVY_REDRAW_MIN_MS like any other. */
    if (holdForContract) {
        const msg = "Loading...";
        print(Math.max(0, (128 - text_width(msg)) >> 1), 28, msg, 1);
        return true;
    }

    /* draw_line / draw_circle / fill_circle (src/host/js_display.c) do the
     * whole shape in C — one QuickJS<->native crossing regardless of length,
     * unlike the per-pixel fillRect a JS-side Bresenham/circle walk needs.
     * viz_draw.mjs and render_page_movy.mjs use them when present; this is
     * where they're offered. `draw_circle` is a one-pixel OUTLINE and is what
     * the knob ring wants; `fill_circle` is a solid disk. They are not
     * interchangeable — subtracting one disk from another does not give a
     * ring (see render_page_movy.mjs drawArcKnob). */
    /* THE LAYOUT IS SELECTED HERE, at the page-draw call site — one value handed
     * to the controller, not a mode flag woven through the renderer. Re-applied
     * every frame so toggling Param View takes effect without re-entering. */
    controller.setLayout(paramPagesLayout());
    traced("js.grid.draw", () => controller.render(
        {
            fillRect: fill_rect, print, textWidth: text_width, line: draw_line,
            fillCircle: fill_circle,
            drawCircle: typeof draw_circle === "function" ? draw_circle : undefined,
            drawArc: typeof draw_arc === "function" ? draw_arc : undefined,
        },
        { title: headerTitle(), footer: footerHints() }
    ));
    return true;
}

/* MIDI events/sec and knob-turns/sec, same standing diagnostic as the fps
 * counter above and logged the same way (once/sec, only under
 * debug_log_on). This is what actually found the flood: a fast physical
 * spin decodes to 250-320 CC messages/second, all as knob turns. */
let _midiWindowStart = 0, _midiCount = 0, _knobTurnCount = 0;

/**
 * Hardware MIDI. Returns true when the event was consumed.
 *
 * Every decision here is in page_input.mjs; this routes the result and performs
 * the two things the controller cannot do for itself.
 */
export function handleParamPagesMidi(data) {
    if (!controller) return false;

    const nowMsProbe = Date.now();
    _midiCount++;
    if (!_midiWindowStart) _midiWindowStart = nowMsProbe;
    else if (nowMsProbe - _midiWindowStart >= 1000) {
        console.log(`param_pages midi: ${_midiCount} events (${_knobTurnCount} knob turns) / ${nowMsProbe - _midiWindowStart}ms`);
        _midiWindowStart = nowMsProbe;
        _midiCount = 0;
        _knobTurnCount = 0;
    }

    /* Mute (CC 88) IS forwarded, and shadow_ui.js already tracks it for the
     * Mute+JogClick bypass shortcut — so read its state rather than keeping a
     * second copy that could disagree. */
    const intent = decodeInput(data, {
        shift: shiftIsHeld(),
        mute: typeof ctx.isMuteHeld === 'function' ? !!ctx.isMuteHeld() : false,
    });
    if (!intent) return false;
    if (intent.type === 'knob') _knobTurnCount++;

    /*
     * Touch trace: every touch edge as the view received it, with its arrival
     * time and the order.
     *
     * Kept after the reset gesture it was built to debug was dropped, because
     * it is the JS-side half of a pair: `touch_trace_on` in the shim records
     * the same edges in the SPI callback, and comparing the two is what proved
     * this path faithful to within 7ms. Any future "the UI missed my input"
     * question starts here.
     *
     * Costs a native "is logging on?" check per touch event when off.
     */
    if (intent.type === 'touch' && isLoggingEnabled()) {
        const st = controller.state;
        const prev = st.lastTouchMs || 0;
        const gap = prev ? (Date.now() - prev) : -1;
        st.lastTouchMs = Date.now();
        log('param_pages', `touch slot=${intent.slot} ${intent.down ? 'DOWN' : 'up  '}`
            + ` t=${Date.now()}`
            + ` sincePrev=${gap < 0 ? 'n/a' : gap + 'ms'}`
            + ` held=[${st.touchOrder.join(',')}]`);
    }

    /* reveal:false — this host drives reveal from the polled shift state in
     * tickParamPages, not from an intent it will never see. */
    const todo = traced("js.grid.input",
        () => applyInput(controller, intent, { nowMs: Date.now(), reveal: false }));

    if (!todo) return true;

    if (todo.action === 'exit') {
        /* Back to the editor you came IN through. Read BEFORE exitParamPages so
         * the destination cannot depend on what the teardown leaves behind. */
        const chrome = currentChrome;
        const back = (chrome && chrome.returnView) || ctx.VIEWS.CHAIN_EDIT;
        exitParamPages();
        /* A contract that is the TOP of its own stack has no view to go back
         * to — Global Settings' Back leaves shadow mode entirely — so it hands
         * in an onExit instead of a returnView. Expressed as a callback rather
         * than a sentinel view id because "leave shadow mode" is not a view and
         * pretending it is one would need a fake case in every switch. */
        if (chrome && typeof chrome.onExit === 'function') chrome.onExit();
        else ctx.setView(back);
        return true;
    }
    if (todo.action === 'menu') {
        /* A menu entry was activated. The controller never performs an action —
         * the host owns what Save or Knob Mapping means — so this only forwards
         * which one was chosen, and the host runs the same code the list runs. */
        const entry = todo.entry || {};
        if (entry.action) {
            /*
             * A synthesised contract may carry its OWN runner, and Master FX
             * settings has to: the generic host runner takes the IPC SLOT, and
             * Master FX is addressed at IPC slot 0 by convention — so "save"
             * from the master bus would have saved instrument slot 1's patch.
             * The io is the only thing in this file that knows which contract
             * is loaded, which is why the choice is made from it rather than
             * from a test on the component name.
             */
            if (controllerIo && typeof controllerIo.runAction === 'function') {
                controllerIo.runAction(entry.action);
            } else if (typeof ctx.runSlotAction === 'function') {
                ctx.runSlotAction(currentSlot, entry.action);
            }
        }
        return true;
    }
    if (todo.action === 'open') {
        /*
         * An ENUM opens the option picker, and it does NOT go the long way
         * round through the list editor.
         *
         * The other divable types hand off to a screen that only exists inside
         * the hierarchy editor, so openParamEditor has to exit the grid, enter
         * that editor and find the param again. An option list needs none of
         * that — it needs the options and an index, both of which the intent is
         * carrying — so the grid CONTROLLER STAYS ALIVE and the page and cell
         * survive by construction, exactly as the LFO target picker does.
         *
         * That is also the only thing that makes this work on SLOT SETTINGS and
         * MASTER FX SETTINGS, which are synthesised contracts with no
         * ui_hierarchy to enter and whose enums (Recv Ch, Fwd Ch, MPE) are the
         * ones with the longest option lists on the device. The commit goes
         * back through the controller so the slot io's own mappings — Fwd's
         * offset, MPE's compound write — are applied rather than bypassed.
         */
        if (Array.isArray(todo.options) && todo.options.length > 0 &&
            typeof ctx.openEnumPicker === 'function') {
            /* The note-off for the held knob will go to the PICKER, so drop the
             * touch now or the cell stays highlighted after we come back. */
            clearParamPagesTouch();
            const key = todo.key;
            ctx.openEnumPicker({
                title: todo.meta && (todo.meta.label || todo.meta.name) || key,
                options: todo.options,
                index: todo.index || 0,
                commit: (i) => { if (controller) controller.commitEnum(key, i); },
                returnToGrid: true,
            });
            return true;
        }
        /* A filepath, canvas, wav_position or string param: hand it to the
         * editor the list view already has rather than building a second one. */
        if (typeof ctx.openParamEditor === 'function') {
            ctx.openParamEditor(currentSlot, todo.fullKey, todo.meta);
        }
        return true;
    }
    return true;
}

/** Read the page aloud — the gesture that stands in for a glance. */
export function announceParamPageContents() {
    if (controller) controller.announceContents();
}

/**
 * Forget any held knob.
 *
 * The grid keeps its controller alive across a hand-off (that is how the page
 * and cell survive coming back), but the note-off for the knob you were holding
 * goes to whatever screen took over and never reaches the grid — so the cell
 * stayed highlighted for good. Holding Target and clicking it is exactly that.
 */
export function clearParamPagesTouch() {
    if (controller && typeof controller.clearTouch === 'function') controller.clearTouch();
}

/** The section picker, for anything that wants to drive it from outside. */
export function paramPagesJumpIndex() {
    return controller ? controller.groupIndex() : [];
}

export function paramPagesGoTo(index) {
    if (controller) controller.goToPage(index);
}

/** True while values are revealed (shift held). */
export function paramPagesRevealing() {
    return !!(controller && controller.state.revealValues);
}

/**
 * The footer hints for the CURRENT state, in draw order.
 *
 * Exported so the ordering rule (jog slot 1, click slot 2) and the per-state
 * wording can be asserted. They cannot be read back off the framebuffer: the
 * footer is set in font4x5, which draws glyphs as fillRect pixels, so a
 * recording print() sees nothing at all.
 */
export function paramPagesFooterHints() {
    return footerHints();
}

/**
 * The footer hints for the ENUM OPTION PICKER.
 *
 * Lives here, next to footerHints(), rather than at the picker's own draw site,
 * because the hint vocabulary is a canon (FOOTER_CANON in render_page_movy.mjs)
 * and a second place that invents wording is how a canon stops being one. The
 * picker is drawn from shadow_ui.js, which is not importable under node, so
 * building the list HERE is also what lets a test read the words at all — the
 * footer is set in font4x5 and cannot be read back off the framebuffer.
 *
 * SEL / SET, not PAGE / OPEN: inside the picker the jog moves the highlight
 * through one param's options and the click writes the highlighted one. BACK is
 * EXIT by the canon, not OUT — it leaves the picker entirely and lands back on
 * whichever editor opened it, exactly as the module picker's BACK does.
 *
 * Constant, so it takes no controller and works from both entry points.
 */
export function enumPickerFooterHints() {
    return orderedHints({ jog: "SEL", click: "SET", extra: [["BACK", "EXIT"]] });
}

/** True while the section picker is over the grid. */
export function paramPagesPickerOpen() {
    return !!(controller && controller.pickerOpen);
}
