/*
 * Text Scroll - Marquee scrolling for long text in menus
 *
 * Tracks scroll state for text that exceeds available width.
 * After a delay, scrolls text to reveal full content.
 * Uses frame counting instead of wall clock for reliability.
 */

/* Frame-based timing (assuming ~60fps tick rate) */
const DEFAULT_DELAY_FRAMES = 120;      /* ~2 seconds before scrolling */
const DEFAULT_SCROLL_INTERVAL = 6;     /* Frames between scroll steps (~100ms) */
const DEFAULT_PAUSE_FRAMES = 120;      /* ~2 seconds pause at end */
const DEFAULT_SCROLL_SPEED = 1;        /* Characters per scroll step */

/**
 * Create a text scroller instance
 * @param {Object} options
 * @param {number} [options.delayFrames=60] - Frames before scrolling starts
 * @param {number} [options.scrollInterval=6] - Frames between scroll steps
 * @param {number} [options.pauseFrames=90] - Frames to pause at end
 * @param {number} [options.scrollSpeed=1] - Characters per scroll step
 * @returns {Object} Text scroller instance
 */
export function createTextScroller(options = {}) {
    const delayFrames = options.delayFrames ?? DEFAULT_DELAY_FRAMES;
    const scrollInterval = options.scrollInterval ?? DEFAULT_SCROLL_INTERVAL;
    const pauseFrames = options.pauseFrames ?? DEFAULT_PAUSE_FRAMES;
    const scrollSpeed = options.scrollSpeed ?? DEFAULT_SCROLL_SPEED;

    let selectedKey = null;
    let framesSinceSelect = 0;
    let framesSinceScroll = 0;
    let framesSincePause = 0;
    let scrollOffset = 0;
    let isPaused = false;
    let textLength = 0;
    let maxChars = 0;

    return {
        /**
         * Update selection - call when selected item changes
         * @param {string|number} key - Unique key for selected item
         */
        setSelected(key) {
            if (key !== selectedKey) {
                selectedKey = key;
                framesSinceSelect = 0;
                framesSinceScroll = 0;
                framesSincePause = 0;
                scrollOffset = 0;
                isPaused = false;
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

            /* Don't check length until we've measured the text */
            if (textLength > 0 && maxChars > 0) {
                if (textLength <= maxChars) {
                    /* Text fits, no scrolling needed */
                    if (scrollOffset !== 0) {
                        scrollOffset = 0;
                        return true;
                    }
                    return false;
                }
            } else {
                /* Haven't measured yet, keep counting frames */
                framesSinceSelect++;
                return false;
            }

            framesSinceSelect++;

            /* Wait for initial delay */
            if (framesSinceSelect < delayFrames) {
                return false;
            }

            /* Handle pause at end */
            if (isPaused) {
                framesSincePause++;
                if (framesSincePause >= pauseFrames) {
                    /* Reset after pause */
                    scrollOffset = 0;
                    isPaused = false;
                    framesSinceSelect = 0;  /* Restart delay */
                    framesSincePause = 0;
                    return true;
                }
                return false;
            }

            framesSinceScroll++;

            /* Check if it's time to scroll */
            if (framesSinceScroll < scrollInterval) {
                return false;
            }

            /* Calculate max scroll offset */
            const maxOffset = textLength - maxChars;
            if (scrollOffset >= maxOffset) {
                /* Reached end, start pause */
                isPaused = true;
                framesSincePause = 0;
                return false;
            }

            /* Scroll */
            scrollOffset = Math.min(scrollOffset + scrollSpeed, maxOffset);
            framesSinceScroll = 0;
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
            framesSinceSelect = 0;
            framesSinceScroll = 0;
            framesSincePause = 0;
            scrollOffset = 0;
            isPaused = false;
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
