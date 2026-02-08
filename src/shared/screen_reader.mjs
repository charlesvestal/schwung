/**
 * Screen Reader Utilities
 *
 * Simple API for announcing UI changes to screen readers.
 * Supports both D-Bus external screen readers and on-device TTS (Flite).
 * Messages are spoken through the Move's audio output.
 */

/**
 * Announce text to the screen reader
 * @param {string} text - Text to announce
 */
export function announce(text) {
    if (!text || typeof text !== 'string') return;

    /* Only announce when Shadow UI is actually visible (display_mode == 1) */
    if (typeof shadow_get_display_mode !== 'undefined') {
        const display_mode = shadow_get_display_mode();
        if (display_mode !== 1) return;  /* Shadow UI not visible, suppress announcement */
    }

    /* Call the host's screen reader function if available */
    if (typeof host_send_screenreader !== 'undefined') {
        host_send_screenreader(text);
    }
}

/**
 * Announce a menu item selection
 * @param {string} label - Item label
 * @param {string} value - Item value (optional)
 */
export function announceMenuItem(label, value) {
    if (!label) return;

    if (value && value !== '') {
        announce(`${label}: ${value}`);
    } else {
        announce(label);
    }
}

/**
 * Announce a parameter change
 * @param {string} paramName - Parameter name
 * @param {string|number} value - Parameter value
 */
export function announceParameter(paramName, value) {
    if (!paramName) return;
    announce(`${paramName}: ${value}`);
}

/**
 * Announce view/mode change
 * @param {string} viewName - New view name
 */
export function announceView(viewName) {
    if (!viewName) return;
    announce(viewName);
}

/**
 * Announce an overlay/message
 * @param {string} message - Overlay message
 */
export function announceOverlay(message) {
    if (!message) return;
    announce(message);
}
