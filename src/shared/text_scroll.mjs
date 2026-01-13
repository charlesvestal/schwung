/*
 * Text Scroll - Marquee scrolling for long text in menus
 *
 * Tracks scroll state for text that exceeds available width.
 * After a delay, scrolls text to reveal full content.
 */

const DEFAULT_DELAY_MS = 1000;      /* Wait before scrolling starts */
const DEFAULT_SCROLL_SPEED = 2;     /* Characters per tick */
const DEFAULT_PAUSE_MS = 1500;      /* Pause at end before resetting */
const DEFAULT_TICK_MS = 100;        /* Time between scroll steps */

/**
 * Create a text scroller instance
 * @param {Object} options
 * @param {number} [options.delayMs=1000] - Delay before scrolling starts
 * @param {number} [options.scrollSpeed=2] - Characters to scroll per step
 * @param {number} [options.pauseMs=1500] - Pause at end before reset
 * @param {number} [options.tickMs=100] - Time between scroll steps
 * @returns {Object} Text scroller instance
 */
export function createTextScroller(options = {}) {
    const delayMs = options.delayMs ?? DEFAULT_DELAY_MS;
    const scrollSpeed = options.scrollSpeed ?? DEFAULT_SCROLL_SPEED;
    const pauseMs = options.pauseMs ?? DEFAULT_PAUSE_MS;
    const tickMs = options.tickMs ?? DEFAULT_TICK_MS;

    let selectedKey = null;
    let selectTime = 0;
    let scrollOffset = 0;
    let lastScrollTime = 0;
    let isPaused = false;
    let pauseStartTime = 0;
    let textLength = 0;
    let maxChars = 0;

    function now() {
        return Date.now ? Date.now() : new Date().getTime();
    }

    return {
        /**
         * Update selection - call when selected item changes
         * @param {string|number} key - Unique key for selected item
         */
        setSelected(key) {
            if (key !== selectedKey) {
                selectedKey = key;
                selectTime = now();
                scrollOffset = 0;
                lastScrollTime = 0;
                isPaused = false;
                pauseStartTime = 0;
            }
        },

        /**
         * Update text info for current selection
         * @param {number} fullLength - Full text length in characters
         * @param {number} visibleChars - Max visible characters
         */
        setTextInfo(fullLength, visibleChars) {
            textLength = fullLength;
            maxChars = visibleChars;
        },

        /**
         * Tick the scroller - call each frame/tick
         * @returns {boolean} True if scroll offset changed
         */
        tick() {
            if (selectedKey === null) return false;
            if (textLength <= maxChars) {
                /* Text fits, no scrolling needed */
                if (scrollOffset !== 0) {
                    scrollOffset = 0;
                    return true;
                }
                return false;
            }

            const currentTime = now();
            const elapsed = currentTime - selectTime;

            /* Wait for initial delay */
            if (elapsed < delayMs) {
                return false;
            }

            /* Handle pause at end */
            if (isPaused) {
                if (currentTime - pauseStartTime >= pauseMs) {
                    /* Reset after pause */
                    scrollOffset = 0;
                    isPaused = false;
                    selectTime = currentTime; /* Restart delay */
                    return true;
                }
                return false;
            }

            /* Check if it's time to scroll */
            if (currentTime - lastScrollTime < tickMs) {
                return false;
            }

            /* Calculate max scroll offset */
            const maxOffset = textLength - maxChars;
            if (scrollOffset >= maxOffset) {
                /* Reached end, start pause */
                isPaused = true;
                pauseStartTime = currentTime;
                return false;
            }

            /* Scroll */
            scrollOffset = Math.min(scrollOffset + scrollSpeed, maxOffset);
            lastScrollTime = currentTime;
            return true;
        },

        /**
         * Get current scroll offset
         * @returns {number} Character offset for text display
         */
        getOffset() {
            return scrollOffset;
        },

        /**
         * Get scrolled text
         * @param {string} text - Full text
         * @param {number} visibleChars - Max visible characters
         * @returns {string} Text with scroll offset applied
         */
        getScrolledText(text, visibleChars) {
            if (!text || text.length <= visibleChars) {
                return text;
            }
            this.setTextInfo(text.length, visibleChars);
            return text.substring(scrollOffset, scrollOffset + visibleChars);
        },

        /**
         * Reset scroller state
         */
        reset() {
            selectedKey = null;
            selectTime = 0;
            scrollOffset = 0;
            lastScrollTime = 0;
            isPaused = false;
            pauseStartTime = 0;
        }
    };
}

/**
 * Singleton scroller for menu labels
 */
let menuLabelScroller = null;

export function getMenuLabelScroller() {
    if (!menuLabelScroller) {
        menuLabelScroller = createTextScroller();
    }
    return menuLabelScroller;
}

/**
 * Singleton scroller for menu values
 */
let menuValueScroller = null;

export function getMenuValueScroller() {
    if (!menuValueScroller) {
        menuValueScroller = createTextScroller();
    }
    return menuValueScroller;
}
