/*
 * Menu Stack - Hierarchical navigation history management
 *
 * Tracks the menu navigation stack for hierarchical menus,
 * allowing push/pop navigation with state preservation.
 */

/**
 * Create a new menu stack instance
 * @returns {MenuStack}
 */
export function createMenuStack() {
    const stack = [];

    return {
        /**
         * Push a new menu onto the stack
         * @param {Object} menu - Menu object with title and items
         * @param {string} menu.title - Menu title for display
         * @param {Array} menu.items - Array of menu items
         * @param {number} [menu.selectedIndex=0] - Initial selected index
         */
        push(menu) {
            stack.push({
                title: menu.title,
                items: menu.items,
                selectedIndex: menu.selectedIndex ?? 0
            });
        },

        /**
         * Pop the current menu from the stack
         * @returns {Object|undefined} The popped menu, or undefined if empty
         */
        pop() {
            return stack.pop();
        },

        /**
         * Get the current (top) menu without removing it
         * @returns {Object|null} Current menu or null if empty
         */
        current() {
            return stack.length > 0 ? stack[stack.length - 1] : null;
        },

        /**
         * Get the current stack depth
         * @returns {number} Number of menus on stack
         */
        depth() {
            return stack.length;
        },

        /**
         * Clear the entire stack
         */
        reset() {
            stack.length = 0;
        },

        /**
         * Get breadcrumb path of menu titles
         * @returns {string[]} Array of titles from root to current
         */
        getPath() {
            return stack.map(m => m.title);
        },

        /**
         * Update the selected index of the current menu
         * @param {number} index - New selected index
         */
        setSelectedIndex(index) {
            const current = this.current();
            if (current) {
                current.selectedIndex = index;
            }
        },

        /**
         * Get the selected index of the current menu
         * @returns {number} Selected index, or 0 if no current menu
         */
        getSelectedIndex() {
            const current = this.current();
            return current ? current.selectedIndex : 0;
        }
    };
}
