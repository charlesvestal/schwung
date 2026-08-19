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
import { createController } from '/data/UserData/schwung/shared/param_pages/page_controller.mjs';
import { decodeInput, applyInput } from '/data/UserData/schwung/shared/param_pages/page_input.mjs';
import { PAGE_KNOBS } from '/data/UserData/schwung/shared/param_pages/page_plan.mjs';
import { LAYOUT_BAR, LAYOUT_DIAL } from '/data/UserData/schwung/shared/param_pages/render_page.mjs';
import { LAYOUT_MOVY } from '/data/UserData/schwung/shared/param_pages/render_page_movy.mjs';
import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';

/* The live controller, or null when the view is not open. One at a time: the
 * grid always shows a single component, and rebuilding on entry is cheap. */
let controller = null;
let currentSlot = 0;
let currentComponent = 'synth';

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
 * Whether the knob grid should be used at all.
 *
 * The screen reader forces the list regardless of the setting. A grid has eight
 * cells and nothing selected, so it is only navigable by ear once the announce
 * calls below are proven on hardware — until then the list, whose reading order
 * is its navigation order, stays the accessible surface.
 */
export function paramPagesEnabled() {
    if (typeof tts_get_enabled === 'function' && tts_get_enabled()) return false;
    const mode = typeof param_view_get_mode === 'function' ? param_view_get_mode() : PARAM_VIEW_LIST;
    return mode === PARAM_VIEW_KNOBS;
}

/**
 * Point the grid at a component. Safe to call on every entry — the controller
 * rebuilds only when the declared contract actually changed.
 *
 * @param {number} slot        chain slot 0-3
 * @param {string} component   'synth' | 'fx1' | 'fx2' | 'midiFx' | 'master_fx:fx1' …
 * @param {string} prefix      the DSP param prefix for that component
 */
/* "Once per session" (see showHint below) has to live here, not in the
 * controller's own state: exitParamPages() drops `controller` on every exit
 * (chain edit, switching modules), so `if (!controller)` below builds a
 * BRAND NEW one on the next entry, and a per-controller "already shown" flag
 * resets right along with it — the hint was popping up on every single
 * module open instead of once, which is what it looks like without this. */
let hintShownThisSession = false;

export function enterParamPages(slot, component, prefix) {
    currentSlot = slot;
    currentComponent = component;

    if (!controller) {
        controller = createController({
            getParam: (key) => ctx.getSlotParam(currentSlot, key),
            setParam: (key, value) => ctx.setSlotParam(currentSlot, key, value),
            announce,
            /* The list editor marks these with "~"; the grid ticks the cell. */
            isModulated: (key) => (typeof ctx.isParamModulated === 'function'
                ? !!ctx.isParamModulated(currentSlot, key) : false),
        });
    }
    /* Entering the view is the only way the module behind it can have changed,
     * so this is where the cached abbreviation is dropped. */
    _abbrevCache = null;
    controller.load({ slot, component, prefix: prefix || component, visible: ctx.evaluateVisibilityCondition });
    /* "Knobs" IS schwung-movy's own knob-page layout now, not Schwung's
     * earlier dial/bar grid — see render_page_movy.mjs. The setting stays a
     * plain List/Knobs toggle; this is what "Knobs" draws. */
    controller.setLayout(LAYOUT_MOVY);
    /* Once per session: the grid's gestures are not guessable, and a preview
     * nobody can operate produces no useful feedback. Any input clears it. */
    if (!hintShownThisSession) {
        hintShownThisSession = true;
        /* ~19 characters fit at 5x7 across the panel; longer lines silently clip. */
        controller.showHint([
            "Jog: page",
            "Shift+Jog: section",
            "Click: section list",
            "Hold knob: name",
            "Shift: fine + values",
            "Mute+knob: default",
        ], "Param Pages");
    }
    ctx.setView(ctx.VIEWS.PARAM_PAGES);
}

export function exitParamPages() {
    controller = null;
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
    _loadingPoll = (_loadingPoll + 1) % LOADING_POLL_TICKS;
    if (_loadingPoll === 0) {
        const loading = ctx.getSlotParam(currentSlot, `${currentComponent}:is_loading`) === '1';
        if (!loading && wasLoading) controller.reloadIfChanged({ visible: ctx.evaluateVisibilityCondition });
        wasLoading = loading;
    }

    /* Shift is polled, not evented (see shiftIsHeld), so reveal follows it here
     * rather than on a CC that never arrives. */
    controller.setReveal(shiftIsHeld());

    controller.tick();
}
let wasLoading = false;
/* is_loading is an edge that fires once per module load; polling it every tick
 * cost a full IPC round trip per frame. See tickParamPages. */
const LOADING_POLL_TICKS = 8;
let _loadingPoll = 0;
/* Module id per (slot, component), read once instead of on every draw — it
 * changes only on a module swap, which goes through openParamPages. */
let _abbrevCache = null;

/**
 * A ~30fps ceiling on the Movy-style grid's own full redraw, independent of
 * shadow_ui.js's global `needsRedraw` gate.
 *
 * That gate gives idle frames a throttle (REDRAW_INTERVAL) but drops it the
 * moment `needsRedraw` is true — which a knob turn sets on nearly every tick.
 * A fast turn therefore demands the MOST frequent redraws at exactly the
 * moment each one is also the MOST expensive (a live envelope/filter/lfo/eq
 * curve recomputing every tick, real per-pixel geometry — see viz_draw.mjs):
 * the two compound instead of one smoothing the other, which is what read as
 * "lower fps" specifically on fast turns and not on slow ones. This throttle
 * is local to the grid rather than a change to the shared gate other views
 * also depend on — it returns `true` (handled) on a skipped tick without
 * drawing, so the previous frame's pixels simply persist for a few
 * milliseconds rather than the view falling back to the list editor.
 */
const MOVY_REDRAW_MIN_MS = 32;
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

/** Draw. Non-grid pages are not ours — the host dispatches those. */
export function drawParamPages() {
    if (!controller) return false;
    /* The section picker is drawn over whatever page you were on, including a
     * non-grid one, so it is checked before the page kind. */
    const page = controller.page;
    if (!controller.pickerOpen && (!page || page.kind !== PAGE_KNOBS)) return false;

    const nowMs = Date.now();
    if (nowMs - lastDrawMs < MOVY_REDRAW_MIN_MS) return true;
    lastDrawMs = nowMs;

    _fpsCount++;
    if (!_fpsWindowStart) _fpsWindowStart = nowMs;
    else if (nowMs - _fpsWindowStart >= 1000) {
        console.log(`param_pages fps: ${_fpsCount} draws / ${nowMs - _fpsWindowStart}ms`);
        _fpsWindowStart = nowMs;
        _fpsCount = 0;
    }

    clear_screen();
    /* Cached: this was a synchronous round trip on EVERY draw (1.4 of the
     * grid's 7.1 reads per tick, measured on device) to render a two-letter
     * abbreviation that cannot change without going back through
     * openParamPages, which clears the cache. */
    if (_abbrevCache === null) {
        _abbrevCache = ctx.getModuleAbbrev
            ? ctx.getModuleAbbrev(ctx.getSlotParam(currentSlot, `${currentComponent}_module`) || '')
            : currentComponent.toUpperCase();
    }
    const abbrev = _abbrevCache;
    /* A hardware synth puts the PATCH name in its display, not the model
     * number — and the module's identity is already visible in the chain
     * editor you came from. Falls back to the abbreviation until the read
     * cursor has picked the name up, and for modules with no presets. */
    const name = controller.presetName || abbrev;

    /* draw_line / draw_circle / fill_circle (src/host/js_display.c) do the
     * whole shape in C — one QuickJS<->native crossing regardless of length,
     * unlike the per-pixel fillRect a JS-side Bresenham/circle walk needs.
     * viz_draw.mjs and render_page_movy.mjs use them when present; this is
     * where they're offered. `draw_circle` is a one-pixel OUTLINE and is what
     * the knob ring wants; `fill_circle` is a solid disk. They are not
     * interchangeable — subtracting one disk from another does not give a
     * ring (see render_page_movy.mjs drawArcKnob). */
    controller.render(
        {
            fillRect: fill_rect, print, textWidth: text_width, line: draw_line,
            fillCircle: fill_circle,
            drawCircle: typeof draw_circle === "function" ? draw_circle : undefined,
            drawArc: typeof draw_arc === "function" ? draw_arc : undefined,
        },
        { title: `S${currentSlot + 1} > ${name}` }
    );
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

    /* reveal:false — this host drives reveal from the polled shift state in
     * tickParamPages, not from an intent it will never see. */
    const todo = applyInput(controller, intent, { nowMs: Date.now(), reveal: false });
    if (!todo) return true;

    if (todo.action === 'exit') {
        exitParamPages();
        ctx.setView(ctx.VIEWS.CHAIN_EDIT);
        return true;
    }
    if (todo.action === 'open') {
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

/** Layout preference, for the settings menu. */
export function setParamPagesLayout(layout) {
    if (controller) controller.setLayout(layout === 'bar' ? LAYOUT_BAR : LAYOUT_DIAL);
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

/** True while the section picker is over the grid. */
export function paramPagesPickerOpen() {
    return !!(controller && controller.pickerOpen);
}
