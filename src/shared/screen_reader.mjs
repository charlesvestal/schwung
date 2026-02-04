/**
 * Screen Reader Utilities - On-device TTS
 *
 * Simple API for announcing UI changes via on-device text-to-speech.
 * Messages are spoken through the Move's audio output via espeak-ng.
 */

/**
 * Announce text to the screen reader
 * @param {string} text - Text to announce
 */
export function announce(text) {
    if (!text || typeof text !== 'string') return;

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
