/*
 * layout_map.mjs -- the MAP navigation layout: all sixteen knobs are
 * parameters (two pages at once), a Shift HOLD shows the slot map, Shift+turn
 * pages, and a Shift TAP is the Mixer. It began as the E16's navigation and is
 * now one of the two layouts every surface can run (layout_knobs.mjs is the
 * other); see layout_common.mjs for the contract a layout keeps.
 *
 * createNav is the gesture half -- what a press, a turn and Shift MEAN -- and
 * is tested on its own (test_e16_nav.sh). createMapLayout wraps it with the
 * parameter, Mixer and reading half.
 */
/*
 * ---------------------------------------------------------------------------
 * NAVIGATION -- the Shift-held map, and what the sixteen buttons mean.
 *
 * =================== THE MODIFIER CANNOT BE A LATCH =========================
 *
 * There is exactly one piece of modifier state here and it is a TIMESTAMP, not
 * a boolean. `mapVisible(now)` is COMPUTED from it on every read; nothing
 * anywhere stores "the map is up".
 *
 * That shape is chosen against a specific, expensive failure. CLAUDE.md's
 * account of `pad_block` is the precedent: a flag that could only be lowered by
 * an event stuck on a field device for THIRTEEN HOURS across two shim inits,
 * and enumerating the ways it could end failed on hardware TWICE -- once for a
 * jump that keeps the module loaded, once for co-run. The conclusion recorded
 * there is that such a flag is released by an INVARIANT restated every frame,
 * never by an exit list.
 *
 * Shift is the same problem with a worse channel. The note-off is one MIDI
 * message on a USB-A port that is known to drop whole packets under traffic
 * (docs/E16_REMOTE.md; 34 packets amid other traffic lost 8), the E16 has no
 * battery so unplugging it silently power-cycles it mid-hold, and the shim's
 * forwarding gate can be closed while a finger is down. Every one of those is a
 * note-on with no note-off, and NONE of them generates an event we could add to
 * an exit list -- which is precisely why the escape cannot be one.
 *
 * So the hold EXPIRES. `MAP_MAX_HOLD_MS` after the press, `mapVisible` answers
 * false because time moved, with no message, no tick, and no cooperation from
 * anything. There is no state that could be left behind, because the only
 * state is a number being compared against a clock that only goes forwards.
 * The map is momentary by design -- it exists while the modifier is held and
 * there is no mode to be lost in -- and this keeps that true even when the
 * device stops talking to us mid-gesture.
 *
 * Two consequences worth stating so they are not later "fixed":
 *
 *   - A DOWN WHILE THE MAP IS ALREADY UP DOES NOT RE-ARM IT. Refreshing the
 *     deadline from input would make a stranded hold immortal for exactly the
 *     user who is trying to work through it: a hand turning knobs would keep
 *     feeding the very state that is eating its turns.
 *   - A LOWER PUSH CONSUMES THE HOLD. The jump ends the gesture, so the surface
 *     you land on is the one you can see. It is also the common-case escape:
 *     most strandings end on the next thing the user does anyway.
 *
 * `tick()` is the restatement. It compares the DERIVED visibility against what
 * was last actually drawn and invalidates on a difference -- so the expiry
 * repaints itself with no event, the same way `reconcilePadBlock()` restates
 * its flag every frame rather than trusting the exits.
 *
 * =================== WHY THE CURRENT SLOT'S OWN CELL IS THE BUS DOOR ========
 *
 * The design asks for "a dedicated cell" that swaps the lower twelve to buses,
 * and `buildMap` leaves none: all sixteen are slots or components. Stealing one
 * of the twelve would shrink the component capacity of every slot to pay for a
 * view most slots have nothing to show in. So the door is the top-row cell of
 * the slot you are ALREADY on -- a press there cannot mean "switch to this
 * slot", which makes it free, and it reads as the slot's own handle: press it
 * to change what is listed beneath it, turn it to page that list.
 *
 * =================== FOLLOW FOCUS: ONE VARIABLE, TWO SOURCES ===============
 *
 * With follow on, Move's screen writes the focus and the E16 mirrors it. The
 * mode is deliberately narrow, and the narrowness is the design:
 *
 *   - ONE FOCUS VARIABLE. Follow does not add a focus; it decides who writes
 *     the one there is. Two owners is a surface that disagrees with itself.
 *   - THE MAP IS DISABLED. That is what stops the two surfaces fighting, and
 *     it is what lets the setting mean one sentence -- is the E16 showing what
 *     Move shows, or its own thing? A mode where both can navigate has no
 *     answer to "who wins", so there is no such mode.
 *   - ONE-WAY. Nothing on this side ever reports focus back: navigating the
 *     E16 must never move Move's screen. In code that is simply "applyFollow
 *     does not call onFocus", and it is pinned in tests/host because the
 *     tempting future edit -- telling the caller what we are showing, for
 *     symmetry -- would read as helpful.
 *   - OFF RESTORES, IT DOES NOT RESET. Follow is something you switch on to
 *     look at something; `parked` is what you come back to.
 *
 * Everything is pure and injected, like the rest of this file: the chain, the
 * page count, the parameter renderer, the follow source and the focus callback
 * are all the caller's. This module never reads or writes a parameter -- a
 * jump is REPORTED through `onFocus`, and `e16_view.mjs` remains the only path
 * a value can travel.
 * ---------------------------------------------------------------------------
 */
import { buildMap } from "./e16_map.mjs";
import { renderMap, pageStep } from "./e16_view.mjs";
import { SHIFT_TAP_MS, createFocus } from "./surface_core.mjs";

/* How long a Shift hold can live without a note-off.
 *
 * Ten seconds is a judgement, and the direction of the error is the point. The
 * map is a jump-target picker: every hold that produces a jump ends when the
 * jump does, so a hold that has produced nothing for ten seconds is far more
 * likely to be a lost note-off than a decision. Erring long strands the
 * surface; erring short costs one more press. */
export const MAP_MAX_HOLD_MS = 10000;

/*
 * THE MAP APPEARS AFTER A SHORT HOLD, NOT ON THE PRESS.
 *
 * Shift is also the page modifier: Shift+turn steps the parameter pages. With
 * the map drawn on the press, every page turn flashed the whole map first (a
 * view switch and back -- two repaints for a gesture that wanted neither).
 * Now the map is drawn only once Shift has been held this long with nothing
 * turned; a turn inside the hold pages the parameters and SUPPRESSES the map
 * for the rest of that hold. A push acts as if the map were up -- the slot
 * row is a fixed position, so Shift+top-row switches slot without waiting to
 * see it.
 */
export const MAP_SHOW_DELAY_MS = 400;

/*
 * A TAP OF SHIFT TOGGLES THE MIXER (e16_mixer.mjs) -- the same grammar as every
 * other surface (surface_core.mjs, SHIFT_TAP_MS): released within that long,
 * having done nothing. A tap, not a held state: nothing is left latched on a
 * lost note-off, which is the whole reason the map is a hold. A press that did
 * anything (a turn, a push, a map) is not a tap. It was a DOUBLE tap while the
 * E16 was the only surface; the EC4, which has no map on its hold, made the
 * single tap the natural gesture, and one grammar across devices won.
 */

/* The top row is the four slots; everything below is the selected slot's
 * content. Both halves of that split are already `e16_map.mjs`'s, and this is
 * the input side of the same fact. */
const SLOT_CELLS = 4;

export function createNav(opts) {
    const o = opts || {};
    const display = o.display || null;
    const chainOf = o.chainOf || (() => ({ slots: [] }));
    const onFocus = o.onFocus || (() => {});
    const renderParams = o.renderParams || (() => {});
    const pageCountOf = o.pageCountOf || (() => 1);
    const maxHoldMs = o.maxHoldMs === undefined ? MAP_MAX_HOLD_MS : o.maxHoldMs;
    const showDelayMs = o.showDelayMs === undefined ? MAP_SHOW_DELAY_MS : o.showDelayMs;
    /*
     * FOLLOW FOCUS -- the second source for the ONE focus variable below.
     *
     * `followFocusOf()` answers what Move's own screen is showing, as
     * `{ slot, component }`, or NULL when it could not answer. There is no
     * second focus here and there must never be one: `slot` / `component` /
     * `pageIndex` are the surface's focus whichever source is writing them, and
     * a mode with its own copy is a surface that disagrees with itself -- the
     * rings drawn for one component, the screen for another, with nothing
     * logged because both halves are behaving correctly.
     */
    const followFocusOf = o.followFocusOf || (() => null);
    /* THE focus (surface_core createFocus): owned by the surface and handed
     * in, so the nav, the controller binding and the host see one. A nav
     * built alone (tests) makes its own. */
    const focus = o.focus || createFocus({
        chainOf, followFocusOf, slot: o.slot, component: o.component, follow: o.follow,
    });

    /* THE ONLY MODIFIER STATE. Null means no hold; a number is when it began.
     * Never a boolean -- see the header. */
    let shiftDownAt = null;
    /* A page turn happened during this hold: the map stays hidden until the
     * next press (see MAP_SHOW_DELAY_MS). */
    let turnedThisHold = false;
    /* The Mixer view is up (a tap of Shift; see SHIFT_TAP_MS). */
    const renderMixerView = o.renderMixer || null;
    let mixerOn = false;
    let actedThisHold = false;

    let mapPage = 0;
    let showBuses = false;

    /* What the last framebuffer that actually went out was showing. Compared
     * against the derived visibility in tick(); it is a record of the PAST, so
     * it cannot be the thing that decides the present. */
    let shownMap = false;

    /*
     * THE MAP IS DISABLED WHILE FOLLOW IS ON, and it is disabled HERE rather
     * than at each gesture.
     *
     * Every branch in handle() already asks this one question, so one term
     * closes all of them at once -- a push under a held Shift falls through to
     * the ordinary `click` path, a Shift+turn to the ordinary `turn`. Gating
     * the gestures individually instead would leave whichever branch was
     * written next as a way to navigate while following, and that is the mode
     * with no answer to "who wins": Move drives the E16, and the E16 must not
     * be able to drive back.
     */
    /* The modifier is HELD (gestures mean map things) ... */
    const held = (now) =>
        !focus.follow && shiftDownAt !== null && (now - shiftDownAt) < maxHoldMs;
    /* ... and the map is SHOWN once it has been held MAP_SHOW_DELAY_MS with
     * no page turn (see MAP_SHOW_DELAY_MS). Both derived from the timestamp,
     * so the delay needs no timer: tick() notices the change and repaints. */
    const mapVisible = (now) =>
        held(now) && !turnedThisHold && !mixerOn && (now - shiftDownAt) >= showDelayMs;

    const invalidate = () => { if (display) display.invalidate(); };

    /*
     * Take one reading from the follow source.
     *
     * A NULL IS NOT A PLAN. CLAUDE.md's tri-state rule: a read that did not
     * complete must never become a default. The source is consulted every
     * frame, so collapsing null into "slot 0, synth" would drag the surface to
     * slot 0 on any tick the shadow UI could not answer and drag it back on the
     * next -- which reads as a flickering surface rather than as a failed read.
     *
     * It reports NOTHING through onFocus. That silence is the one-way rule:
     * onFocus is the only channel this module has to move anything outside
     * itself, and firing it here would be Move's screen jumping because the
     * E16 mirrored Move's screen.
     */
    const applyFollow = () => {
        if (focus.poll()) invalidate();
    };

    const currentMap = () =>
        buildMap(chainOf(), { slot: focus.slot, page: mapPage, showBuses });

    return {
        /**
         * One surface event -- whatever `e16_input.decode()` produced.
         *
         * @returns a description of what happened, or null for an event that
         *          meant nothing here. A push or turn arriving while the map is
         *          DOWN is handed straight back as `click` / `turn` for the
         *          caller to route through the grid: this module decides what a
         *          gesture MEANS, never what a parameter becomes.
         */
        handle(ev, now) {
            if (!ev) return null;

            if (ev.type === "shift") {
                /* The modifier itself is inert while following. `mapVisible`
                 * already answers false, but without this the DOWN edge would
                 * still arm `shiftDownAt` and repaint -- and the hold would
                 * then be live the instant follow was switched off, with the
                 * map appearing under a finger that is no longer on Shift. */
                if (focus.follow) return null;
                if (ev.down) {
                    /* Deliberately not a re-arm: a repeat down while up leaves
                     * the original deadline standing (see the header). */
                    if (held(now)) return null;
                    shiftDownAt = now;
                    turnedThisHold = false;
                    actedThisHold = false;
                    mapPage = 0;
                    showBuses = false;
                    /* With a delay, no repaint yet: the map is drawn after
                     * MAP_SHOW_DELAY_MS, by tick() noticing it is due. */
                    if (mapVisible(now)) { invalidate(); return { action: "map" }; }
                    return { action: "hold" };
                }
                const wasShown = mapVisible(now);
                /* A TAP: released quickly, having done nothing. It toggles
                 * the Mixer. */
                const tap = !!renderMixerView && !actedThisHold && !wasShown &&
                            shiftDownAt !== null && now - shiftDownAt < SHIFT_TAP_MS;
                shiftDownAt = null;
                turnedThisHold = false;
                if (tap) {
                    mixerOn = !mixerOn;
                    invalidate();
                    return { action: "mixer", on: mixerOn };
                }
                /* Only a map that was actually up needs taking down. A tap, a
                 * hold spent paging, or a hold that already ended (a jump, an
                 * expiry) leaves a screen that is already correct. */
                if (!wasShown) return null;
                invalidate();
                return { action: "params" };
            }

            /* A button release is inert. Pushes act on the DOWN edge, so acting
             * on the up edge too would run every gesture twice -- and a jump
             * run twice is a second onFocus for a component the user selected
             * once. */
            if (ev.type === "release") return null;

            if (ev.type === "push") {
                if (mixerOn && !mapVisible(now)) {
                    /* THE MIXER: a push, or Shift+push (solo, 100%). The
                     * Shift+push spends the hold -- no map under it. */
                    const sh = held(now);
                    if (sh) { turnedThisHold = true; actedThisHold = true; }
                    return { action: "mixerPush", enc: ev.enc, shift: sh };
                }
                if (held(now)) actedThisHold = true;
                if (!held(now)) return { action: "click", enc: ev.enc };
                if (!mapVisible(now)) {
                    /* Held, map not drawn yet (or suppressed by a page turn):
                     * a push asks for the map, so it appears NOW and the push
                     * acts on it -- a blind slot switch would change nothing
                     * the knobs drive, since that takes a module pick. */
                    turnedThisHold = false;
                    shiftDownAt = Math.min(shiftDownAt, now - showDelayMs);
                    invalidate();
                }
                const enc = ev.enc | 0;
                if (enc < SLOT_CELLS) {
                    if (enc === focus.slot) {
                        /* The bus view of a slot with no buses is an empty
                         * list -- every lower knob went dark, which read as a
                         * broken page (hardware, 2026-09-24). Only a slot that
                         * HAS buses toggles; otherwise the tap is inert. */
                        const hasBuses = buildMap(chainOf(), { slot: focus.slot, showBuses: true }).cells.slice(SLOT_CELLS).some(Boolean);
                        if (!showBuses && !hasBuses) return null;
                        showBuses = !showBuses;
                        mapPage = 0;
                        invalidate();
                        return { action: "buses", showBuses };
                    }
                    /* The slot is entered at the module last edited there
                     * (createFocus) -- never at the old slot's position name,
                     * which on this slot may be empty or another module. */
                    focus.enterSlot(enc);
                    mapPage = 0;
                    /* Leaving bus view on a slot change is not tidiness: the
                     * new slot's components would otherwise be hidden behind a
                     * mode the user set while looking at a different slot. */
                    showBuses = false;
                    invalidate();
                    return { action: "slot", slot: focus.slot };
                }
                const cell = currentMap().cells[enc];
                /* A hole is not a destination (e16_map.mjs). Nothing moves and
                 * nothing repaints -- in particular the hold is NOT consumed,
                 * or a mis-hit would drop the map the user is still reading. */
                if (!cell) return null;
                focus.set(cell.slot, cell.component);
                shiftDownAt = null;   /* the jump ends the gesture */
                mixerOn = false;      /* ...and lands on the module's knobs */
                onFocus(focus.slot, focus.component);
                invalidate();
                return { action: "focus", slot: focus.slot, component: focus.component };
            }

            if (ev.type === "turn") {
                if (mixerOn && !mapVisible(now)) {
                    /* THE MIXER: a turn, or Shift+turn (pan) -- which keeps
                     * the hold alive and the map hidden, as paging does. */
                    const sh = held(now);
                    if (sh) { turnedThisHold = true; actedThisHold = true; shiftDownAt = now; }
                    return { action: "mixerTurn", enc: ev.enc, ticks: ev.ticks, shift: sh };
                }
                if (!held(now)) {
                    return { action: "turn", enc: ev.enc, ticks: ev.ticks };
                }
                actedThisHold = true;
                if (!mapVisible(now)) {
                    /* SHIFT+TURN BEFORE THE MAP SHOWS PAGES THE PARAMETERS,
                     * from any encoder, and keeps the map hidden for the rest
                     * of this hold -- the hand is paging, not looking for a
                     * module. */
                    turnedThisHold = true;
                    /* PAGING KEEPS THE HOLD ALIVE. A Shift+turn is a
                     * legitimate reason to hold Shift for a long time, and
                     * the MAP_MAX_HOLD_MS expiry ended it mid-turn (hardware,
                     * 2026-09-24: "it lost the shift hold"). Each page step
                     * restarts the expiry, so it now means "10 s after the
                     * last turn". A stranded Shift stays escapable: the turn
                     * glyph shows it is held, and a tap of Shift clears it.
                     * (Map turns still do NOT re-arm -- see the header.) */
                    shiftDownAt = now;
                    /* A layout may scale a device's pulses into page steps
                     * (a coarse encoder pages by angle): zero steps is a turn
                     * that paged nothing, but it still spent the hold. */
                    if (!ev.ticks) return { action: "page", pageIndex: focus.pageIndex };
                    const next = pageStep(focus.pageIndex, ev.ticks, pageCountOf());
                    if (!focus.setPage(next)) return { action: "page", pageIndex: focus.pageIndex };
                    invalidate();
                    return { action: "page", pageIndex: focus.pageIndex };
                }
                if (!ev.ticks) return null;
                if ((ev.enc | 0) < SLOT_CELLS) {
                    /* The slot row owns the map's own list, so turning it pages
                     * that list. Splitting the two paging axes by WHICH encoder
                     * moved keeps both available at once; deciding by whether
                     * the map happens to overflow would make one gesture mean
                     * two things depending on the rig. */
                    const count = currentMap().pageCount;
                    const next = Math.max(0, Math.min(count - 1,
                        mapPage + (ev.ticks > 0 ? 1 : -1)));
                    if (next === mapPage) return { action: "mapPage", mapPage };
                    mapPage = next;
                    invalidate();
                    return { action: "mapPage", mapPage };
                }
                const next = pageStep(focus.pageIndex, ev.ticks, pageCountOf());
                if (!focus.setPage(next)) return { action: "page", pageIndex: focus.pageIndex };
                invalidate();
                return { action: "page", pageIndex: focus.pageIndex };
            }

            return null;
        },

        /**
         * Restate the invariant. Call every frame, before the display's tick.
         *
         * This is the stranded-modifier escape made visible: the hold can end
         * with no event, so something has to notice that the screen no longer
         * matches the derived state. It compares against what was last DRAWN
         * rather than tracking transitions, so it cannot miss one -- a
         * transition counter has to be right at every site that changes the
         * state, and this has to be right once.
         *
         * (It reconciles what THIS process has drawn. A device still showing a
         * frame from a previous process is the lifecycle's problem, not this
         * one's -- see createLifecycle.)
         */
        /**
         * Switch the focus SOURCE. Idempotent, and the idempotence is not
         * tidiness: the park happens on the OFF->ON edge only, so a
         * setFollow(true) that parked unconditionally would, on its second
         * call, park the FOLLOW source as if it were the user's own focus --
         * and the user's real focus would be gone with no gesture that could
         * bring it back.
         */
        setFollow(on, now) {
            /* The park and the restore are the focus's (createFocus). */
            if (!focus.setFollow(on)) return;
            /* A hold in progress cannot survive: the map is gone, so its
             * modifier would be a note-off owed to a view that no longer
             * exists. The timestamp shape means dropping it is the whole job
             * -- there is no latch anywhere else to unwind. */
            if (focus.follow) shiftDownAt = null;
            invalidate();
        },

        tick(now) {
            /* Follow is a POLL, not a subscription: shadow_ui.js does not tell
             * anyone when its component changes, and adding a notification for
             * this one consumer would be a second thing to keep in step with
             * every path that moves that focus (see reconcileCcClaim, which
             * re-derives the same tuple for the same reason). Reading it is
             * cheap -- the shadow UI already holds it -- and applyFollow
             * repaints only on a CHANGE, so an unchanged source costs nothing
             * on the wire. */
            applyFollow();
            if (mapVisible(now) !== shownMap) invalidate();
        },

        /**
         * Draw whichever view is current. Handed to the display's tick as the
         * frame producer, so it runs ONLY when a repaint is actually going out.
         */
        render(ctx, now) {
            const up = mapVisible(now);
            if (up) renderMap(ctx, currentMap(), { page: mapPage, showBuses });
            else if (mixerOn && renderMixerView) renderMixerView(ctx);
            else renderParams(ctx);
            shownMap = up;
        },

        mapVisible,
        /** Shift is held (a gesture now means a map thing). */
        held,
        /** The device drew the current view without render() (it draws from
         *  the layout's screen model); the stranded-map check needs to know. */
        markDrawn(now) { shownMap = mapVisible(now); },
        /** Leave the Mixer (a layout switch lands on the parameters). */
        closeMixer() { if (mixerOn) { mixerOn = false; invalidate(); } },

        get slot() { return focus.slot; },
        get component() { return focus.component; },
        get pageIndex() { return focus.pageIndex; },
        get mapPage() { return mapPage; },
        /** The slot map as it stands (whether or not it is on screen). */
        map() { return currentMap(); },
        get showBuses() { return showBuses; },
        get followEnabled() { return focus.follow; },
        get focus() { return focus; },
        /* The DISPLAY MODE depends on this: a map is a picture and the
         * parameter view is sixteen labels, and the two modes override each
         * other on the device. Exposed rather than re-derived at the call site
         * -- the hold has a timeout, so "is the map up" is a question only this
         * object can answer correctly. */
        mapVisible(now) { return mapVisible(now); },
        /** Shift is held and the knob view is still showing: a turn pages. */
        turnHint(now) { return !mixerOn && held(now) && !mapVisible(now); },
        /** The Mixer view is up. */
        get mixer() { return mixerOn; },
    };
}


/*
 * ---------------------------------------------------------------------------
 * THE MAP LAYOUT -- the nav above, plus what a turn and a push DO.
 *
 * Moved here from the E16's assembly unchanged in behaviour: a turn goes
 * through the grid's own knob path (applyTurn), ONE ring is restated, the
 * digits follow as a paced repaint (valueMoved), and the title owes a redraw.
 * What is new is only what a coarser device needs: pulses are converted
 * through the device's knob feel, a Shift+turn pages by the device's selector
 * angle, and each movement leaves a READING for a device that can show one.
 * ---------------------------------------------------------------------------
 */
import { buildView, ringFor, ringsFor, mapRings, moduleRgb, applyTurn, applyClick } from "./e16_view.mjs";
import { setOrdinal } from "./e16_map.mjs";
import { renderMixer } from "./e16_mixer.mjs";
import { ENUM_DELTA_DIV } from "./knob_engine.mjs";
import { NAV_MAP, NAV_HOLD_MS, isChoice, moduleNameFor, paramReading, pageReading, mixerReading,
         createReadings, controlsText } from "./layout_common.mjs";

/* One Mixer value re-read this often while it is up, to notice changes made
 * elsewhere (Move's track volume, Slot Settings). */
export const MIXER_REFRESH_MS = 250;

export function createMapLayout(ctx) {
    const c = ctx || {};
    const focus = c.focus;
    const binding = c.binding;
    const mixer = c.mixer || null;
    const feel = c.feel;
    const chainOf = c.chainOf || (() => ({ slots: [] }));
    const sel = c.selector || { choice: null, nav: 1, slot: 1 };
    const invalidate = c.invalidate || (() => {});
    const invalidateLabels = c.invalidateLabels || (() => {});
    const ringChanged = c.ringChanged || (() => {});
    const valueMoved = c.valueMoved || (() => {});

    const readings = createReadings();
    let focusEnc = null;
    let mixerRefreshAt = -Infinity;

    const viewNow = () => binding.view(focus.pageIndex);
    const knobRgb = () => moduleRgb(setOrdinal(chainOf(), focus.slot, focus.component));
    const cellRgb = (cell) => moduleRgb(setOrdinal(chainOf(), cell.slot, cell.component));
    const moduleName = () => moduleNameFor(chainOf(), focus.slot, focus.component);
    const slotEmpty = () => !buildMap(chainOf(), { slot: focus.slot }).cells.slice(4).some(Boolean);

    const nav = createNav({
        display: { invalidate },
        focus,
        chainOf,
        pageCountOf: () => Math.max(1, binding.knobPages().length),
        renderMixer: mixer ? (cv) => renderMixer(cv, mixer) : null,
        onFocus: () => {},
    });

    /* A device pulse count -> what this turn is worth, by what it drives. */
    function paramDetents(enc, pulses) {
        const cell = viewNow().cells[enc];
        if (sel.choice && cell && isChoice(cell.meta)) return feel.steps(enc, pulses, sel.choice) * ENUM_DELTA_DIV;
        return feel.detents(enc, pulses);
    }

    function pageNames() { return binding.knobPages().map((p) => String(p.name || "")); }

    return {
        name: NAV_MAP,
        nav,
        get mixerOn() { return !!(mixer && nav.mixer); },

        handle(ev, t) {
            if (!ev) return null;
            if (ev.type === "turn") {
                feel.begin(ev.enc, t);
                /* Held Shift outside the Mixer: the turn is a page or map-page
                 * step, so a coarse device steps it by angle. */
                if (nav.held(t) && !nav.mixer) {
                    const perStep = nav.mapVisible(t) && (ev.enc | 0) < 4 ? sel.slot : sel.nav;
                    ev = { type: "turn", enc: ev.enc, ticks: feel.steps(ev.enc, ev.ticks, perStep) };
                }
            }
            const before = focus.pageIndex;
            const act = nav.handle(ev, t);
            if (!act) return null;

            if (act.action === "page" && act.pageIndex !== before) {
                readings.show(pageReading(moduleName() || "Module", pageNames(), focus.pageIndex), t, NAV_HOLD_MS);
                return act;
            }
            if (mixer) {
                if (act.action === "mixer") {
                    /* Entering reads the whole Mixer once (~18 round trips);
                     * after that our own writes keep it, and tick() re-reads
                     * one value at a time. */
                    if (act.on) mixer.load();
                    readings.clear();
                    return act;
                }
                if (act.action === "mixerTurn" || act.action === "mixerPush") {
                    const changed = act.action === "mixerTurn"
                        ? feel.mixerTurn(mixer, act.enc, feel.detents(act.enc, act.ticks), act.shift, t)
                        : mixer.push(act.enc, act.shift);
                    if (changed) {
                        /* The ring at once; the digits (and a MUTE/SOLO label)
                         * follow as a live repaint, as a turn does. */
                        ringChanged(mixer.ringFor(act.enc));
                        valueMoved(t);
                        readings.show(mixerReading(mixer, act.enc, act.action === "mixerTurn" && act.shift), t);
                    }
                    return act;
                }
            }
            const ctl = binding.controller;
            if (!ctl) return act;

            if (act.action === "turn") {
                const d = paramDetents(act.enc, act.ticks);
                const moved = d ? applyTurn(viewNow(), ctl, act.enc, d, t) : null;
                /* ONE RING, NEVER A REPAINT: a knob under a hand makes one of
                 * these per detent (docs/E16_REMOTE.md). The view is rebuilt
                 * after the write so the ring carries the new value. */
                if (moved) {
                    const view = viewNow();
                    ringChanged(ringFor(view, act.enc, knobRgb()));
                    valueMoved(t);
                    /* The title is the only place a full name and reading fit
                     * on a text screen: owed, at a lower priority than the ring. */
                    invalidateLabels();
                    const cell = view.cells[act.enc];
                    if (cell) readings.show(paramReading(cell, (view.headers[cell.half] || {}).name || "", binding.metaOf), t);
                }
                focusEnc = act.enc;
                return act;
            }
            if (act.action === "click") {
                const pageBefore = ctl.pageIndex;
                const hit = applyClick(viewNow(), ctl, act.enc);
                /* A click flips a value (a ring) or opens a door (a page). */
                if (ctl.pageIndex !== pageBefore) invalidate();
                else if (hit) {
                    const view = viewNow();
                    ringChanged(ringFor(view, act.enc, knobRgb()));
                    invalidateLabels();
                    const cell = view.cells[act.enc];
                    if (cell) readings.show(paramReading(cell, (view.headers[cell.half] || {}).name || "", binding.metaOf), t);
                }
                focusEnc = act.enc;
                return act;
            }
            return act;
        },

        tick(t) {
            nav.tick(t);
            if (mixer && nav.mixer && !nav.mapVisible(t) && t - mixerRefreshAt >= MIXER_REFRESH_MS) {
                mixerRefreshAt = t;
                mixer.refreshNext();
            }
        },

        screen(t) {
            if (nav.mapVisible(t)) {
                return { kind: "map", map: nav.map(), page: nav.mapPage, showBuses: nav.showBuses, slot: focus.slot };
            }
            if (mixer && nav.mixer) return { kind: "mixer", mixer, alt: nav.held(t) };
            if (slotEmpty()) return { kind: "empty", slot: focus.slot };
            const controls = binding.controls();
            if (controls !== "ok") {
                return { kind: "message", slot: focus.slot, component: moduleName(), text: controlsText(controls) };
            }
            return { kind: "params", view: viewNow(), component: moduleName(), turnHint: nav.turnHint(t), focusEnc };
        },

        /* ALL SIXTEEN, dark ones included: a device that keeps whatever a
         * ring last showed would otherwise keep the view you just left. */
        rings(t) {
            if (nav.mapVisible(t)) return mapRings(nav.map(), cellRgb);
            if (mixer && nav.mixer) return mixer.rings();
            return slotEmpty() ? ringsFor(null) : ringsFor(viewNow(), knobRgb());
        },
        ring(enc) { return ringFor(viewNow(), enc, knobRgb()); },
        /** The parameter view the knobs drive (both pages). */
        view: viewNow,

        context(t) {
            return [NAV_MAP, nav.mapVisible(t), nav.mixer, focus.slot, focus.component, focus.pageIndex,
                    nav.mapPage, nav.showBuses, binding.controls()].join("|");
        },
        reading(t) { return readings.at(t); },
        turnHint(t) { return nav.turnHint(t); },
        markDrawn(t) { nav.markDrawn(t); },
        setFollow(on, t) { nav.setFollow(on, t); },
        /* Leaving this layout: drop the Mixer and any hold. */
        reset() { nav.closeMixer(); readings.clear(); },
    };
}
