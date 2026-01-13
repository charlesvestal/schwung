/*
 * Menu Navigation - Input handling for hierarchical menus
 *
 * Handles jog wheel, click, arrows, and back button for menu navigation
 * with support for value editing mode.
 */

import { MenuItemType, isEditable, isSubmenu, isBack } from './menu_items.mjs';

/* CC values from constants.mjs */
const CC_JOG_WHEEL = 14;
const CC_JOG_CLICK = 3;
const CC_BACK = 51;
const CC_LEFT = 62;
const CC_RIGHT = 63;
const CC_UP = 55;
const CC_DOWN = 54;

/**
 * Create menu navigation state
 * @returns {Object} Navigation state object
 */
export function createMenuState() {
    return {
        selectedIndex: 0,
        editing: false,
        editValue: null
    };
}

/**
 * Handle menu input from MIDI CC
 * @param {Object} options
 * @param {number} options.cc - MIDI CC number
 * @param {number} options.value - CC value
 * @param {Array} options.items - Current menu items
 * @param {Object} options.state - Menu state (from createMenuState)
 * @param {Object} options.stack - Menu stack (from createMenuStack)
 * @param {Function} [options.onBack] - Callback when backing out of root menu
 * @param {boolean} [options.shiftHeld=false] - Whether shift is held (for fine adjust)
 * @returns {Object} Result with needsRedraw flag
 */
export function handleMenuInput({ cc, value, items, state, stack, onBack, shiftHeld = false }) {
    const isDown = value > 0;
    const item = items[state.selectedIndex];
    let needsRedraw = false;

    /* Jog wheel - scroll or adjust value */
    if (cc === CC_JOG_WHEEL) {
        const delta = decodeDelta(value);
        if (delta !== 0) {
            if (state.editing) {
                needsRedraw = adjustValue(item, state, delta, shiftHeld);
            } else {
                const newIndex = clamp(state.selectedIndex + delta, 0, items.length - 1);
                if (newIndex !== state.selectedIndex) {
                    state.selectedIndex = newIndex;
                    needsRedraw = true;
                }
            }
        }
    }

    /* Jog click - enter submenu, start/confirm edit, execute action */
    if (cc === CC_JOG_CLICK && isDown) {
        needsRedraw = handleClick(item, state, stack, onBack);
    }

    /* Up/Down arrows - scroll list */
    if (cc === CC_UP && isDown) {
        if (!state.editing) {
            const newIndex = Math.max(state.selectedIndex - 1, 0);
            if (newIndex !== state.selectedIndex) {
                state.selectedIndex = newIndex;
                needsRedraw = true;
            }
        }
    }
    if (cc === CC_DOWN && isDown) {
        if (!state.editing) {
            const newIndex = Math.min(state.selectedIndex + 1, items.length - 1);
            if (newIndex !== state.selectedIndex) {
                state.selectedIndex = newIndex;
                needsRedraw = true;
            }
        }
    }

    /* Left/Right arrows - quick adjust value without entering edit mode */
    if ((cc === CC_LEFT || cc === CC_RIGHT) && isDown) {
        const delta = cc === CC_RIGHT ? 1 : -1;
        if (state.editing) {
            needsRedraw = adjustValue(item, state, delta, shiftHeld);
        } else if (isEditable(item)) {
            /* Quick adjust - change value immediately without edit mode */
            needsRedraw = applyValueChange(item, delta, shiftHeld);
        }
    }

    /* Back button - cancel edit or go back in menu stack */
    if (cc === CC_BACK && isDown) {
        if (state.editing) {
            /* Cancel edit, restore original value */
            state.editing = false;
            state.editValue = null;
            needsRedraw = true;
        } else if (stack && stack.depth() > 1) {
            /* Pop menu stack */
            stack.pop();
            const current = stack.current();
            if (current) {
                state.selectedIndex = current.selectedIndex || 0;
            }
            needsRedraw = true;
        } else if (onBack) {
            /* At root, call onBack callback */
            onBack();
            needsRedraw = true;
        }
    }

    return { needsRedraw };
}

/**
 * Handle jog click on a menu item
 */
function handleClick(item, state, stack, onBack) {
    if (!item) return false;

    switch (item.type) {
        case MenuItemType.SUBMENU:
            if (stack && item.getMenu) {
                const subItems = item.getMenu();
                stack.push({
                    title: item.label,
                    items: subItems,
                    selectedIndex: 0
                });
                state.selectedIndex = 0;
            }
            return true;

        case MenuItemType.VALUE:
        case MenuItemType.ENUM:
            if (state.editing) {
                /* Confirm edit - apply value */
                if (item.set && state.editValue !== null) {
                    item.set(state.editValue);
                }
                state.editing = false;
                state.editValue = null;
            } else {
                /* Start edit */
                state.editing = true;
                state.editValue = item.get ? item.get() : null;
            }
            return true;

        case MenuItemType.TOGGLE:
            /* Toggle immediately on click */
            if (item.get && item.set) {
                item.set(!item.get());
            }
            return true;

        case MenuItemType.ACTION:
            if (item.onAction) {
                item.onAction();
            }
            return true;

        case MenuItemType.BACK:
            if (stack && stack.depth() > 1) {
                stack.pop();
                const current = stack.current();
                if (current) {
                    state.selectedIndex = current.selectedIndex || 0;
                }
            } else if (onBack) {
                onBack();
            }
            return true;

        default:
            return false;
    }
}

/**
 * Adjust value while in edit mode (changes editValue, not actual value)
 */
function adjustValue(item, state, delta, shiftHeld) {
    if (!item || !state.editing) return false;

    const currentVal = state.editValue;
    let newVal;

    if (item.type === MenuItemType.VALUE) {
        const step = shiftHeld ? (item.fineStep || 1) : (item.step || 1);
        newVal = clamp(currentVal + delta * step, item.min, item.max);
    } else if (item.type === MenuItemType.ENUM) {
        const opts = item.options || [];
        if (opts.length === 0) return false;
        const idx = opts.indexOf(currentVal);
        const newIdx = (idx + delta + opts.length) % opts.length;
        newVal = opts[newIdx];
    } else {
        return false;
    }

    if (newVal !== state.editValue) {
        state.editValue = newVal;
        return true;
    }
    return false;
}

/**
 * Apply value change immediately (for quick adjust with arrows)
 */
function applyValueChange(item, delta, shiftHeld) {
    if (!item || !isEditable(item)) return false;
    if (!item.get || !item.set) return false;

    const currentVal = item.get();
    let newVal;

    if (item.type === MenuItemType.VALUE) {
        const step = shiftHeld ? (item.fineStep || 1) : (item.step || 1);
        newVal = clamp(currentVal + delta * step, item.min, item.max);
    } else if (item.type === MenuItemType.ENUM) {
        const opts = item.options || [];
        if (opts.length === 0) return false;
        const idx = opts.indexOf(currentVal);
        const newIdx = (idx + delta + opts.length) % opts.length;
        newVal = opts[newIdx];
    } else if (item.type === MenuItemType.TOGGLE) {
        newVal = !currentVal;
    } else {
        return false;
    }

    if (newVal !== currentVal) {
        item.set(newVal);
        return true;
    }
    return false;
}

/**
 * Decode jog wheel delta from CC value
 * @param {number} value - CC value (1-63 = clockwise, 65-127 = counter-clockwise)
 * @returns {number} Delta (-1, 0, or 1)
 */
function decodeDelta(value) {
    if (value === 0) return 0;
    if (value >= 1 && value <= 63) return 1;
    if (value >= 65 && value <= 127) return -1;
    return 0;
}

/**
 * Clamp value between min and max
 */
function clamp(val, min, max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}
