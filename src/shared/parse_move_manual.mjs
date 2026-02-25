/**
 * Parse Ableton Move Manual HTML into help viewer format.
 *
 * Fetches the manual from ableton.com, parses headings and text content
 * into a hierarchical JSON structure, and caches the result on disk.
 */

const MANUAL_URL = "https://www.ableton.com/en/move/manual/";
const CACHE_DIR = "/data/UserData/move-anything/cache";
const CACHE_PATH = CACHE_DIR + "/move_manual.json";
const HTML_PATH = "/data/UserData/move-anything/cache/move_manual.html";
const MAX_LINE_WIDTH = 20;
const CACHE_MAX_AGE_DAYS = 1;
const CACHE_VERSION = 2; /* Bump when notice text or parsing logic changes */

function wrapText(text, maxChars) {
    if (!text) return [];
    const words = text.split(/\s+/).filter(w => w.length > 0);
    const lines = [];
    let current = '';
    for (const word of words) {
        if (current.length === 0) {
            current = word;
        } else if (current.length + 1 + word.length <= maxChars) {
            current += ' ' + word;
        } else {
            lines.push(current);
            current = word;
        }
    }
    if (current) lines.push(current);
    return lines;
}

function sanitizeText(text) {
    return text
        .replace(/[\u2018\u2019\u201A]/g, "'")
        .replace(/[\u201C\u201D\u201E]/g, '"')
        .replace(/\u2026/g, '...')
        .replace(/[\u2013\u2014]/g, '-')
        .replace(/\u00A9/g, '(c)')
        .replace(/\u00D7/g, 'x')
        .replace(/[^\x00-\x7E]/g, '');
}

function stripHtml(html) {
    return html
        .replace(/<br\s*\/?>/gi, '\n')
        .replace(/<\/p>/gi, '\n\n')
        .replace(/<\/li>/gi, '\n')
        .replace(/<li[^>]*>/gi, '- ')
        .replace(/<[^>]+>/g, '')
        .replace(/&amp;/g, '&')
        .replace(/&lt;/g, '<')
        .replace(/&gt;/g, '>')
        .replace(/&quot;/g, '"')
        .replace(/&#39;/g, "'")
        .replace(/&nbsp;/g, ' ')
        .replace(/\u00A0/g, ' ')
        .replace(/\n{3,}/g, '\n\n')
        .trim();
}

function parseHtml(html) {
    /* Find the main content area */
    const contentStart = html.indexOf('data-number="1"');
    if (contentStart === -1) return null;

    const contentEnd = html.indexOf('</section>', html.lastIndexOf('data-number='));
    const content = html.substring(contentStart - 100, contentEnd > 0 ? contentEnd + 50 : html.length);

    /* Extract headings with data-number attribute */
    const headingRegex = /<h([1-4])\s[^>]*data-number="([^"]*)"[^>]*>[^<]*<span[^>]*>[^<]*<\/span>\s*([\s\S]*?)<\/h\1>/g;

    const matches = [];
    let match;
    while ((match = headingRegex.exec(content)) !== null) {
        matches.push({
            level: parseInt(match[1]),
            number: match[2],
            title: sanitizeText(stripHtml(match[3]).trim()),
            index: match.index,
            endIndex: match.index + match[0].length
        });
    }

    /* Extract text between headings */
    for (let i = 0; i < matches.length; i++) {
        const startIdx = matches[i].endIndex;
        const endIdx = (i + 1 < matches.length) ? matches[i + 1].index : content.length;
        const rawContent = content.substring(startIdx, endIdx);
        const textContent = rawContent
            .replace(/<aside[\s\S]*?<\/aside>/gi, '')
            .replace(/<figure[\s\S]*?<\/figure>/gi, '')
            .replace(/<nav[\s\S]*?<\/nav>/gi, '');
        matches[i].text = sanitizeText(stripHtml(textContent).trim());
    }

    return matches;
}

function buildHierarchy(flatSections) {
    /* Build a proper tree with children arrays at every level.
     * When a node has both text and children, text moves to an "Overview" pseudo-child. */
    const chapters = [];
    const stack = [];

    for (const section of flatSections) {
        const depth = section.number.indexOf('.') === -1 ? 1 :
            section.number.split('.').filter(s => s.length > 0).length;

        const node = { title: section.title };
        if (section.text) {
            node.lines = wrapText(section.text, MAX_LINE_WIDTH);
        }

        if (depth === 1) {
            chapters.push(node);
            stack.length = 0;
            stack[1] = node;
        } else {
            const parent = stack[depth - 1];
            if (parent) {
                if (!parent.children) parent.children = [];
                parent.children.push(node);
            }
            stack[depth] = node;
            stack.length = depth + 1;
        }
    }

    /* Post-process: when a node has both lines and children,
     * move lines into an "Overview" pseudo-child at the front.
     * Also remove empty leaf nodes (no lines, no children) such as
     * image-only sections like "Move Controls". */
    function postProcess(node) {
        if (node.children) {
            if (node.lines && node.lines.length > 0) {
                node.children.unshift({ title: "Overview", lines: node.lines });
                delete node.lines;
            }
            for (const child of node.children) {
                postProcess(child);
            }
            /* Remove empty leaf children */
            node.children = node.children.filter(
                c => (c.lines && c.lines.length > 0) || (c.children && c.children.length > 0)
            );
            /* If all children were removed, drop the array */
            if (node.children.length === 0) delete node.children;
        }
    }

    for (const chapter of chapters) {
        postProcess(chapter);
    }

    return chapters;
}

/**
 * Check if cached manual data is stale (older than CACHE_MAX_AGE_DAYS).
 */
function isCacheStale(cacheData) {
    if (!cacheData || !cacheData.fetched) return true;
    /* Version mismatch means notice or parsing changed — re-fetch */
    if (cacheData.version !== CACHE_VERSION) return true;
    try {
        const fetched = new Date(cacheData.fetched).getTime();
        const now = Date.now();
        const ageDays = (now - fetched) / (1000 * 60 * 60 * 24);
        return ageDays > CACHE_MAX_AGE_DAYS;
    } catch (e) {
        return true;
    }
}

/**
 * Try to read cached manual data. Returns the parsed cache object or null.
 */
function readCache() {
    if (typeof host_file_exists !== 'function' || !host_file_exists(CACHE_PATH)) return null;
    try {
        const cached = host_read_file(CACHE_PATH);
        if (cached) {
            const data = JSON.parse(cached);
            if (data && data.sections && data.sections.length > 0) {
                return data;
            }
        }
    } catch (e) { /* corrupt */ }
    return null;
}

/**
 * Write cache data to disk using host_write_file / host_ensure_dir.
 */
function writeCache(cacheData) {
    try {
        if (typeof host_ensure_dir === 'function') {
            host_ensure_dir(CACHE_DIR);
        }
        if (typeof host_write_file === 'function') {
            host_write_file(CACHE_PATH, JSON.stringify(cacheData));
        }
    } catch (e) { /* cache write failed, non-fatal */ }
}

/**
 * Fetch and parse the Move Manual from ableton.com.
 * Returns sections array on success, null on failure.
 */
function fetchAndParse() {
    if (typeof host_http_download !== 'function') return null;

    const ok = host_http_download(MANUAL_URL, HTML_PATH);
    if (!ok) return null;

    const html = host_read_file(HTML_PATH);
    if (!html) return null;

    const flatSections = parseHtml(html);
    if (!flatSections || flatSections.length === 0) return null;

    const hierarchy = buildHierarchy(flatSections);

    const cacheData = {
        fetched: new Date().toISOString(),
        version: CACHE_VERSION,
        sections: hierarchy
    };

    writeCache(cacheData);
    return cacheData.sections;
}

/**
 * Get the Move Manual sections, using cache if fresh, fetching if stale/missing.
 * When cache is stale, attempts a refresh; falls back to stale data if offline.
 * Returns sections array or null.
 */
export function fetchAndParseManual() {
    const cached = readCache();
    if (cached && !isCacheStale(cached)) {
        return cached.sections;
    }
    /* Stale cache exists - return it immediately, fetch next time.
     * This avoids blocking the UI on a slow/failing HTTP request. */
    if (cached) {
        return cached.sections;
    }
    /* No cache at all - must fetch (first-time only) */
    const sections = fetchAndParse();
    if (sections) return sections;
    return null;
}

/**
 * Refresh the cached manual (re-fetch from web). Returns sections or null.
 */
export function refreshManual() {
    return fetchAndParse();
}

/**
 * Clear the cached manual to force re-fetch.
 */
export function clearManualCache() {
    try {
        if (typeof host_write_file === 'function') {
            host_write_file(CACHE_PATH, '');
        }
    } catch (e) {}
}
