/**
 * Unified logging module for Move Anything
 *
 * Usage:
 *   import { log, debug, info, warn, error } from './logger.mjs';
 *   log('MyModule', 'Something happened');
 *   debug('MyModule', 'Debug info');
 *
 * Or override console.log:
 *   import { installConsoleOverride } from './logger.mjs';
 *   installConsoleOverride('MyModule');
 *   console.log('Now goes to unified log');
 */

/* Check if unified_log binding exists (shadow UI context) */
const hasUnifiedLog = typeof unified_log === 'function';
const hasUnifiedLogEnabled = typeof unified_log_enabled === 'function';

/* Fallback source name */
let defaultSource = 'js';

/**
 * Check if logging is enabled
 */
export function isLoggingEnabled() {
    if (hasUnifiedLogEnabled) {
        return unified_log_enabled();
    }
    return false;
}

/**
 * Log a message with source prefix
 */
export function log(source, message) {
    if (!isLoggingEnabled()) return;

    if (hasUnifiedLog) {
        unified_log(source || defaultSource, String(message));
    } else {
        /* Fallback to console for non-shadow contexts */
        console.log(`[${source}] ${message}`);
    }
}

/**
 * Convenience functions matching log levels
 */
export function debug(source, message) {
    log(source, message);
}

export function info(source, message) {
    log(source, message);
}

export function warn(source, message) {
    log(source, `WARN: ${message}`);
}

export function error(source, message) {
    log(source, `ERROR: ${message}`);
}

/**
 * Install console.log override to route to unified log
 * Call this once at module startup with your module name
 */
export function installConsoleOverride(moduleName) {
    defaultSource = moduleName || 'js';

    if (typeof globalThis !== 'undefined' && hasUnifiedLog) {
        const originalLog = console.log;
        const originalWarn = console.warn;
        const originalError = console.error;

        console.log = function(...args) {
            const message = args.map(a => String(a)).join(' ');
            if (isLoggingEnabled()) {
                unified_log(defaultSource, message);
            }
            /* Also call original for stderr output during development */
            originalLog.apply(console, args);
        };

        console.warn = function(...args) {
            const message = args.map(a => String(a)).join(' ');
            if (isLoggingEnabled()) {
                unified_log(defaultSource, `WARN: ${message}`);
            }
            originalWarn.apply(console, args);
        };

        console.error = function(...args) {
            const message = args.map(a => String(a)).join(' ');
            if (isLoggingEnabled()) {
                unified_log(defaultSource, `ERROR: ${message}`);
            }
            originalError.apply(console, args);
        };
    }
}

export default { log, debug, info, warn, error, isLoggingEnabled, installConsoleOverride };
