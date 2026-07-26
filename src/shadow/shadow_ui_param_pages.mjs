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
import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';

/* The live controller, or null when the view is not open. One at a time: the
 * grid always shows a single component, and rebuilding on entry is cheap. */
let controller = null;
let shiftHeld = false;
let currentSlot = 0;
let currentComponent = 'synth';

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
    controller.load({ slot, component, prefix: prefix || component, visible: ctx.evaluateVisibilityCondition });
    /* Once per session: the grid's gestures are not guessable, and a preview
     * nobody can operate produces no useful feedback. Any input clears it. */
    /* ~19 characters fit at 5x7 across the panel; longer lines silently clip. */
    controller.showHint([
        "Jog: page",
        "Shift+Jog: section",
        "Click: section list",
        "Hold knob: name",
        "Shift: show values",
    ], "Param Pages");
    shiftHeld = false;
    ctx.setView(ctx.VIEWS.PARAM_PAGES);
}

export function exitParamPages() {
    controller = null;
    shiftHeld = false;
}

export function paramPagesActive() {
    return controller !== null;
}

/** Which component the grid is pointed at, for handing back to the list. */
export function paramPagesComponent() {
    return currentComponent;
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
     * reset values and the cursor continuously. */
    const loading = ctx.getSlotParam(currentSlot, `${currentComponent}:is_loading`) === '1';
    if (!loading && wasLoading) controller.reloadIfChanged({ visible: ctx.evaluateVisibilityCondition });
    wasLoading = loading;

    controller.tick();
}
let wasLoading = false;

/** Draw. Non-grid pages are not ours — the host dispatches those. */
export function drawParamPages() {
    if (!controller) return false;
    /* The section picker is drawn over whatever page you were on, including a
     * non-grid one, so it is checked before the page kind. */
    const page = controller.page;
    if (!controller.pickerOpen && (!page || page.kind !== PAGE_KNOBS)) return false;

    clear_screen();
    const abbrev = ctx.getModuleAbbrev
        ? ctx.getModuleAbbrev(ctx.getSlotParam(currentSlot, `${currentComponent}_module`) || '')
        : currentComponent.toUpperCase();
    /* A hardware synth puts the PATCH name in its display, not the model
     * number — and the module's identity is already visible in the chain
     * editor you came from. Falls back to the abbreviation until the read
     * cursor has picked the name up, and for modules with no presets. */
    const name = controller.presetName || abbrev;
    controller.render(
        { fillRect: fill_rect, print, textWidth: text_width },
        { title: `S${currentSlot + 1} > ${name}` }
    );
    return true;
}

/**
 * Hardware MIDI. Returns true when the event was consumed.
 *
 * Every decision here is in page_input.mjs; this routes the result and performs
 * the two things the controller cannot do for itself.
 */
export function handleParamPagesMidi(data) {
    if (!controller) return false;

    const intent = decodeInput(data, { shift: shiftHeld });
    if (!intent) return false;
    if (intent.type === 'shift') shiftHeld = intent.down;

    const todo = applyInput(controller, intent, { nowMs: Date.now() });
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

/** True while the section picker is over the grid. */
export function paramPagesPickerOpen() {
    return !!(controller && controller.pickerOpen);
}
