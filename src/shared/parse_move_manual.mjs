/**
 * Parse Ableton Move Manual HTML into help viewer format.
 *
 * Reads from a bundled JSON (shipped in tarball) or a cache JSON (refreshed
 * in background). Never performs blocking HTTP — all network activity is
 * fire-and-forget via host_http_download_background.
 */

const MANUAL_URL = "https://www.ableton.com/en/move/manual/";
const CACHE_DIR = "/data/UserData/move-anything/cache";
const CACHE_PATH = CACHE_DIR + "/move_manual.json";
const HTML_PATH = CACHE_DIR + "/move_manual.html";
const BUNDLED_PATH = "/data/UserData/move-anything/shared/move_manual_bundled.json";
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
 * Try to read a JSON manual file. Returns the parsed object or null.
 */
function readManualJson(path) {
    if (typeof host_file_exists !== 'function' || !host_file_exists(path)) return null;
    try {
        const raw = host_read_file(path);
        if (raw) {
            const data = JSON.parse(raw);
            if (data && data.sections && data.sections.length > 0) {
                return data;
            }
        }
    } catch (e) { /* corrupt or missing */ }
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
 * Get the `fetched` timestamp from a manual data object (ms since epoch), or 0.
 */
function getFetchedTime(data) {
    if (!data || !data.fetched) return 0;
    try { return new Date(data.fetched).getTime(); } catch (e) { return 0; }
}

/**
 * Check if a manual data object is stale (older than CACHE_MAX_AGE_DAYS).
 */
function isStale(data) {
    if (!data || !data.fetched) return true;
    if (data.version !== CACHE_VERSION) return true;
    try {
        const ageDays = (Date.now() - new Date(data.fetched).getTime()) / (1000 * 60 * 60 * 24);
        return ageDays > CACHE_MAX_AGE_DAYS;
    } catch (e) { return true; }
}

/**
 * Get the Move Manual sections — read-only, never blocks on HTTP.
 * Returns the freshest available data from cache or bundled file.
 */
export function fetchAndParseManual() {
    const cached = readManualJson(CACHE_PATH);
    const bundled = readManualJson(BUNDLED_PATH);

    /* Pick whichever has the newer fetched timestamp */
    const cachedTime = getFetchedTime(cached);
    const bundledTime = getFetchedTime(bundled);

    if (cachedTime >= bundledTime && cached) {
        return cached.sections;
    }
    if (bundled) {
        return bundled.sections;
    }
    /* Neither available */
    return null;
}

/**
 * Kick off a background curl to download the manual HTML.
 * Returns immediately — curl runs independently.
 */
export function refreshManualBackground() {
    if (typeof host_http_download_background !== 'function') return;

    /* Only refresh if data is stale */
    const cached = readManualJson(CACHE_PATH);
    const bundled = readManualJson(BUNDLED_PATH);
    const best = (getFetchedTime(cached) >= getFetchedTime(bundled)) ? cached : bundled;
    if (best && !isStale(best)) return;

    try {
        if (typeof host_ensure_dir === 'function') {
            host_ensure_dir(CACHE_DIR);
        }
        host_http_download_background(MANUAL_URL, HTML_PATH);
    } catch (e) { /* non-fatal */ }
}

/**
 * Check if a previously-downloaded HTML file exists and is newer than the
 * cache JSON. If so, parse it and update the cache. Returns true if cache
 * was updated.
 *
 * Called at boot and on help open — just a quick file-exists check in the
 * common case (no HTML file present).
 */
export function processDownloadedHtml() {
    if (typeof host_file_exists !== 'function') return false;
    if (!host_file_exists(HTML_PATH)) return false;

    const html = host_read_file(HTML_PATH);
    if (!html || html.length < 1000) return false;

    const flatSections = parseHtml(html);
    if (!flatSections || flatSections.length === 0) return false;

    const hierarchy = buildHierarchy(flatSections);

    const cacheData = {
        fetched: new Date().toISOString(),
        version: CACHE_VERSION,
        sections: hierarchy
    };

    writeCache(cacheData);

    /* Remove the HTML file so we don't re-parse next time */
    try {
        if (typeof host_write_file === 'function') {
            host_write_file(HTML_PATH, '');
        }
    } catch (e) { /* non-fatal */ }

    return true;
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
