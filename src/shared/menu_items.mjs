/*
 * Menu Items - Standardized menu item type definitions
 *
 * Provides factory functions for creating menu items of different types,
 * ensuring consistent structure across all menus.
 */

export const MenuItemType = {
    SUBMENU: 'submenu',
    VALUE: 'value',
    ENUM: 'enum',
    TOGGLE: 'toggle',
    ACTION: 'action',
    BACK: 'back'
};

/**
 * Create a submenu item that navigates to a child menu
 * @param {string} label - Display label
 * @param {Function} getMenu - Function that returns array of menu items
 * @returns {Object} Submenu item
 */
export function createSubmenu(label, getMenu) {
    return {
        type: MenuItemType.SUBMENU,
        label,
        getMenu
    };
}

/**
 * Create a numeric value item
 * @param {string} label - Display label
 * @param {Object} options - Value options
 * @param {Function} options.get - Getter function returning current value
 * @param {Function} options.set - Setter function receiving new value
 * @param {number} [options.min=0] - Minimum value
 * @param {number} [options.max=127] - Maximum value
 * @param {number} [options.step=1] - Step increment
 * @param {number} [options.fineStep] - Fine step (with shift), defaults to 1
 * @param {Function} [options.format] - Format function for display
 * @returns {Object} Value item
 */
export function createValue(label, { get, set, min = 0, max = 127, step = 1, fineStep, format }) {
    return {
        type: MenuItemType.VALUE,
        label,
        get,
        set,
        min,
        max,
        step,
        fineStep: fineStep ?? 1,
        format
    };
}

/**
 * Create an enum item that cycles through options
 * @param {string} label - Display label
 * @param {Object} options - Enum options
 * @param {Function} options.get - Getter function returning current value
 * @param {Function} options.set - Setter function receiving new value
 * @param {string[]} options.options - Array of option values
 * @param {Function} [options.format] - Format function for display
 * @returns {Object} Enum item
 */
export function createEnum(label, { get, set, options, format }) {
    return {
        type: MenuItemType.ENUM,
        label,
        get,
        set,
        options,
        format
    };
}

/**
 * Create a toggle item (on/off)
 * @param {string} label - Display label
 * @param {Object} options - Toggle options
 * @param {Function} options.get - Getter function returning boolean
 * @param {Function} options.set - Setter function receiving boolean
 * @param {string} [options.onLabel='On'] - Label when on
 * @param {string} [options.offLabel='Off'] - Label when off
 * @returns {Object} Toggle item
 */
export function createToggle(label, { get, set, onLabel = 'On', offLabel = 'Off' }) {
    return {
        type: MenuItemType.TOGGLE,
        label,
        get,
        set,
        onLabel,
        offLabel
    };
}

/**
 * Create an action item that executes a callback
 * @param {string} label - Display label
 * @param {Function} onAction - Callback function
 * @returns {Object} Action item
 */
export function createAction(label, onAction) {
    return {
        type: MenuItemType.ACTION,
        label,
        onAction
    };
}

/**
 * Create a back item that returns to parent menu
 * @param {string} [label='[Back]'] - Display label
 * @returns {Object} Back item
 */
export function createBack(label = '[Back]') {
    return {
        type: MenuItemType.BACK,
        label
    };
}

/**
 * Format a menu item's value for display
 * @param {Object} item - Menu item
 * @param {boolean} [editing=false] - Whether item is being edited
 * @param {*} [editValue] - Temporary edit value
 * @returns {string} Formatted value string
 */
export function formatItemValue(item, editing = false, editValue = null) {
    if (!item) return '';

    const value = editing ? editValue : (item.get ? item.get() : null);

    switch (item.type) {
        case MenuItemType.VALUE:
            if (value === null || value === undefined) return '';
            const formatted = item.format ? item.format(value) : String(value);
            return editing ? `[${formatted}]` : formatted;

        case MenuItemType.ENUM:
            if (value === null || value === undefined) return '';
            const enumFormatted = item.format ? item.format(value) : capitalize(String(value));
            return editing ? `[${enumFormatted}]` : enumFormatted;

        case MenuItemType.TOGGLE:
            const boolVal = editing ? editValue : (item.get ? item.get() : false);
            const toggleText = boolVal ? (item.onLabel || 'On') : (item.offLabel || 'Off');
            return editing ? `[${toggleText}]` : toggleText;

        case MenuItemType.SUBMENU:
            return '>';

        case MenuItemType.ACTION:
        case MenuItemType.BACK:
        default:
            return '';
    }
}

/**
 * Check if an item type is editable
 * @param {Object} item - Menu item
 * @returns {boolean} True if item can be edited
 */
export function isEditable(item) {
    if (!item) return false;
    return item.type === MenuItemType.VALUE ||
           item.type === MenuItemType.ENUM ||
           item.type === MenuItemType.TOGGLE;
}

/**
 * Check if an item type is a submenu
 * @param {Object} item - Menu item
 * @returns {boolean} True if item is a submenu
 */
export function isSubmenu(item) {
    return item && item.type === MenuItemType.SUBMENU;
}

/**
 * Check if an item type is a back button
 * @param {Object} item - Menu item
 * @returns {boolean} True if item is a back button
 */
export function isBack(item) {
    return item && item.type === MenuItemType.BACK;
}

function capitalize(s) {
    if (!s) return '';
    return s.charAt(0).toUpperCase() + s.slice(1);
}
