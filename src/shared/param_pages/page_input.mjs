/**
 * page_input.mjs — Move hardware MIDI → what the knob page should do.
 *
 * Separated from the controller and from shadow_ui.js because this is where
 * the boring bugs live: the wrong CC, a relative encoder decoded as absolute,
 * a modifier that latches. None of that needs a device to get wrong, so none of
 * it should need a device to test.
 *
 * PURE: a message plus modifier state in, an intent out. The caller applies the
 * intent to a controller. Nothing here touches hardware, params or the screen.
 *
 * The map (see CLAUDE.md, "Move Hardware MIDI"):
 *   CC 71-78     knobs 1-8, relative: 1..63 clockwise, 65..127 anticlockwise
 *   CC 14        jog wheel turn, same relative encoding
 *   CC 3         jog click
 *   CC 49        shift  (NOT forwarded in shadow mode — see the host note below)
 *   CC 51        back
 *   CC 88        mute, used here as the modifier for destructive actions
 *   notes 0-7    knob capacitive touch (velocity > 0 = touched)
 */

import { decodeDelta } from "../input_filter.mjs";

export const KNOB_CC_FIRST = 71;
export const KNOB_CC_LAST = 78;
export const JOG_TURN_CC = 14;
export const JOG_CLICK_CC = 3;
export const SHIFT_CC = 49;
export const BACK_CC = 51;
export const MUTE_CC = 88;
export const TOUCH_NOTE_FIRST = 0;
export const TOUCH_NOTE_LAST = 7;
export const PAD_NOTE_FIRST = 68;
export const PAD_NOTE_LAST = 99;

/**
 * Is this message a FINGER hitting a pad?
 *
 * The one thing the grid does with a pad note, and the only one it can: it
 * vouches for the gesture to a module that declared `child_press_param`
 * (docs/MODULES.md, "Live presses"). `decodeInput` returns null for pads on
 * purpose and must keep doing so -- pad typing belongs to the keyboard -- so
 * this is a separate question with a separate answer, not an intent.
 *
 * It lives HERE rather than at the one call site because it is three boring
 * conditions of exactly the kind this file exists to keep testable, and each
 * of them is a bug somebody would otherwise ship unnoticed:
 *
 *   - NOTE-ON ONLY. A pad sends a release too, and vouching for it would
 *     report two presses per hit -- the module would move focus twice, the
 *     second time from a gesture that had already ended.
 *   - A velocity-0 note-on IS a release. Move sends both forms.
 *   - The pad RANGE. Steps (16-31), tracks (40-43) and knob touch (0-9) all
 *     arrive as notes on the same cable, and none of them is a pad.
 *
 * The shim gates the same range before it forwards anything (pad_observe in
 * schwung_shim.c), so in practice this is a second lock on the same door. It
 * is still the one that can be run without a device.
 *
 * @param {number[]|Uint8Array} data  [status, d1, d2]
 * @returns {boolean}
 */
export function isHardwarePadPress(data) {
    if (!data || data.length < 3) return false;
    if ((data[0] & 0xf0) !== 0x90) return false;   /* note-on only */
    if (!(data[2] > 0)) return false;              /* velocity 0 is a release */
    return data[1] >= PAD_NOTE_FIRST && data[1] <= PAD_NOTE_LAST;
}

/**
 * @param {number[]|Uint8Array} data  [status, d1, d2]
 * @param {object} [mods]  { shift: boolean }
 * @returns {object|null} an intent, or null when the message is not ours
 *
 * Intents:
 *   { type: "knob",   slot, direction, fine }  turn knob `slot`
 *   { type: "touch",  slot, down, mute }
 *   { type: "page",   delta, byLevel }
 *   { type: "click",  shift }             jog click — open / commit
 *   { type: "back" }
 *   { type: "shift",  down }              modifier state changed
 *   { type: "mute",   down }              modifier state changed
 */
export function decodeInput(data, mods = {}) {
    if (!data || data.length < 3) return null;
    const status = data[0] & 0xf0;
    const d1 = data[1];
    const d2 = data[2];

    if (status === 0xb0) {
        if (d1 >= KNOB_CC_FIRST && d1 <= KNOB_CC_LAST) {
            const direction = decodeDelta(d2);
            if (direction === 0) return null;
            return { type: "knob", slot: d1 - KNOB_CC_FIRST, direction, fine: !!mods.shift };
        }
        if (d1 === JOG_TURN_CC) {
            const delta = decodeDelta(d2);
            if (delta === 0) return null;
            /* Shift makes the jog coarse — a level at a time rather than a page.
             * On a 76-page module that is the difference between navigating and
             * scrolling. */
            return { type: "page", delta: delta > 0 ? 1 : -1, byLevel: !!mods.shift };
        }
        /* Buttons report press as a non-zero value and release as 0. */
        if (d1 === JOG_CLICK_CC) return d2 > 0 ? { type: "click", shift: !!mods.shift } : null;
        if (d1 === BACK_CC) return d2 > 0 ? { type: "back" } : null;
        if (d1 === SHIFT_CC) return { type: "shift", down: d2 > 0 };
        if (d1 === MUTE_CC) return { type: "mute", down: d2 > 0 };
        return null;
    }

    /* Knob capacitive touch. Move sends note-on with velocity 0 for release as
     * well as note-off, so both spellings must clear the touch or a knob can be
     * left stuck as "held". */
    if (status === 0x90 || status === 0x80) {
        if (d1 >= TOUCH_NOTE_FIRST && d1 <= TOUCH_NOTE_LAST) {
            const down = status === 0x90 && d2 > 0;
            return { type: "touch", slot: d1 - TOUCH_NOTE_FIRST, down, mute: !!mods.mute };
        }
        return null;
    }

    return null;
}

/**
 * Apply an intent to a controller. Returns what the host still has to do
 * itself — leaving the view, or opening an editor the controller cannot own.
 *
 * @returns {object|null} { action: "exit" } | { action: "open", ... } | null
 */
export function applyInput(controller, intent, { nowMs, reveal } = {}) {
    if (!controller || !intent) return null;

    switch (intent.type) {
        case "knob":
            /* Shift is precision mode: it reveals every value AND makes the
             * encoders fine. Chasing a number and being able to read it are the
             * same moment, so they are one modifier and not two. */
            controller.onKnobTurn(intent.slot, intent.direction > 0 ? 1 : -1, nowMs,
                                  { fine: !!intent.fine });
            return null;

        case "touch":
            /*
             * There is deliberately no Mute+touch reset here.
             *
             * It existed, and it could never be advertised: CC 88 is forwarded
             * to Move unconditionally, so holding Mute to reach the gesture
             * also mutes the selected track. A shortcut whose documentation
             * has to warn you what else it does is not a shortcut. It also had
             * a quiet cost — returning early meant the controller never saw
             * the press, so the matching release left a dwell from an older,
             * unrelated contact in the state the double-tap is judged against.
             *
             * Reset lives on the double-tap alone. `intent.mute` is still
             * decoded because other gestures in this view use Mute as a
             * modifier.
             */
            controller.onKnobTouch(intent.slot, intent.down);
            return null;

        case "page":
            controller.onJog(intent.delta, { shift: intent.byLevel });
            return null;

        case "click": {
            if (controller.dismissHint && controller.dismissHint()) return null;
            /*
             * Shift+Click is the section picker, EVERYWHERE.
             *
             * Plain click is contextual — it opens a held param, and on a menu
             * page it enters the menu — which means the page set is not always
             * reachable from it. Shift already means "sections" on the jog, so
             * it means the same at rest, and there is one gesture that always
             * gets you to the pages no matter what is on screen.
             */
            if (intent.shift && !controller.pickerOpen) {
                if (controller.openPicker) controller.openPicker();
                return null;
            }
            /* Two meanings, disambiguated by whether a knob is under your hand.
             * Holding one: open that param's editor (the only unambiguous target
             * on a grid, where nothing is "selected"). Holding none: the click
             * has no target, so it opens the section picker — which is also the
             * only spare gesture, and the thing a 76-page module needs. */
            if (controller.pickerOpen) { controller.pickerSelect(); return null; }
            /* A DOOR page owns the plain click: first press enters it, and on
             * a menu the second activates the highlighted entry. Nothing is
             * "held" on one, so this must come before the no-knob-held branch
             * below or the section picker would swallow it. Shift+click above
             * still reaches the section list from inside a door, which is what
             * keeps one from being a trap.
             *
             * ASK THE CONTROLLER which pages are doors. This used to restate
             * the kinds as a literal list — "menu" || "preset" || "items" — a
             * second definition of a rule the controller already owned. When
             * PAGE_KNOBS became a door in the list layout, only the
             * controller's copy learned it: a plain click on a knobs-as-list
             * page fell past this branch to the no-knob-held one below and
             * opened the SECTION PICKER, so the list could not be entered at
             * all. Nothing failed; the page was simply inert. */
            if (controller.isDoor && controller.isDoor()) {
                const opened = controller.onClick(-1);
                return opened ? controller.takePending() : null;
            }
            const held = controller.state.touched;
            if (held < 0) { controller.openPicker(); return null; }
            const opened = controller.onClick(held);
            return opened ? controller.takePending() : null;
        }

        case "back":
            if (controller.dismissHint && controller.dismissHint()) return null;
            /*
             * THE PEEK IS A LAYER, so Back takes it down and stops there.
             *
             * It went straight through to `exit` and left the module, which
             * reads as a wildly disproportionate response to a panel that was
             * about to disappear on its own — reported from the device as
             * "if i hit back during autopeek it exits the module".
             *
             * It costs nothing to close: the detent has already written, so
             * unlike the picker there is no edit to cancel, and unlike the
             * menu there is no level to step out of. Back here means "I have
             * read it, go away", which is the same thing the timeout means and
             * the same one-layer-at-a-time rule the picker and the menu follow
             * below.
             */
            if (controller.dismissPeek && controller.dismissPeek()) return null;
            /* Back closes the picker first, then steps out of an entered menu,
             * then leaves the view — one layer at a time, matching the rest of
             * Move. A menu you have entered is a layer exactly like the picker
             * is: you went into it, so Back comes out of it. */
            if (controller.pickerOpen) { controller.closePicker(); return null; }
            if (controller.exitMenu && controller.exitMenu()) return null;
            return { action: "exit" };

        case "shift":
            /* Shift doubles as the reveal-values modifier: holding it swaps
             * every label for its value, which is the one thing the dial layout
             * cannot do on its own. */
            if (reveal !== false) controller.setReveal(intent.down);
            return null;

        default:
            return null;
    }
}
