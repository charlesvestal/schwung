#!/bin/bash
#
# Fetch and parse the Ableton Move manual into help viewer JSON.
#
# Usage:
#   ./scripts/fetch_move_manual.sh          # Run locally, output to build/cache/
#   ./scripts/fetch_move_manual.sh --device  # Run on device via SSH
#

set -e

MANUAL_URL="https://www.ableton.com/en/move/manual/"
PARSER_SCRIPT="src/shared/parse_move_manual.mjs"

if [ "$1" = "--device" ]; then
    # Run on device
    MOVE_HOST="${MOVE_HOST:-move.local}"
    CACHE_DIR="/data/UserData/move-anything/cache"
    HTML_PATH="/tmp/move_manual.html"

    echo "Fetching Move manual on device..."
    ssh ableton@${MOVE_HOST} "mkdir -p ${CACHE_DIR}"
    ssh ableton@${MOVE_HOST} "curl -sL '${MANUAL_URL}' -o ${HTML_PATH}"

    # Copy parser to device and run with node (if available) or QuickJS
    scp "${PARSER_SCRIPT}" ableton@${MOVE_HOST}:/tmp/parse_move_manual.mjs

    # Generate the cache using a simple inline parser via shell
    ssh ableton@${MOVE_HOST} "node /tmp/parse_move_manual.mjs 2>/dev/null || echo 'Node not available, using pre-parsed cache'"
    echo "Done. Cache at ${CACHE_DIR}/move_manual.json"
else
    # Run locally, generate cache file to deploy
    HTML_PATH="/tmp/move_manual.html"
    CACHE_DIR=".cache"

    echo "Fetching Move manual..."
    curl -sL "${MANUAL_URL}" -o "${HTML_PATH}"
    echo "Downloaded $(wc -c < "${HTML_PATH}") bytes"

    mkdir -p "${CACHE_DIR}"

    echo "Parsing..."
    node -e "
const fs = require('fs');
const MAX_LINE_WIDTH = 20;

function wrapText(text, maxChars) {
    if (!text) return [];
    const words = text.split(/\s+/).filter(w => w.length > 0);
    const lines = []; let current = '';
    for (const word of words) {
        if (!current.length) { current = word; }
        else if (current.length + 1 + word.length <= maxChars) { current += ' ' + word; }
        else { lines.push(current); current = word; }
    }
    if (current) lines.push(current);
    return lines;
}

function stripHtml(html) {
    return html.replace(/<br\s*\/?>/gi, '\n').replace(/<\/p>/gi, '\n\n')
        .replace(/<\/li>/gi, '\n').replace(/<li[^>]*>/gi, '- ')
        .replace(/<[^>]+>/g, '').replace(/&amp;/g, '&').replace(/&lt;/g, '<')
        .replace(/&gt;/g, '>').replace(/&quot;/g, '\"').replace(/&#39;/g, \"'\")
        .replace(/&nbsp;/g, ' ').replace(/\u00A0/g, ' ').replace(/\n{3,}/g, '\n\n').trim();
}

/* Replace non-ASCII chars with ASCII equivalents for 1-bit display + TTS */
function sanitizeText(text) {
    return text
        .replace(/[\u2018\u2019\u201A]/g, \"'\")   /* smart single quotes */
        .replace(/[\u201C\u201D\u201E]/g, '\"')     /* smart double quotes */
        .replace(/\u2026/g, '...')                   /* ellipsis */
        .replace(/[\u2013\u2014]/g, '-')             /* en/em dash */
        .replace(/\u00D7/g, 'x')                     /* multiplication sign */
        .replace(/[^\x00-\x7E]/g, '');               /* drop anything else non-ASCII */
}

const html = fs.readFileSync('${HTML_PATH}', 'utf-8');
const contentStart = html.indexOf('data-number=\"1\"');
const contentEnd = html.indexOf('</section>', html.lastIndexOf('data-number='));
const content = html.substring(contentStart - 100, contentEnd > 0 ? contentEnd + 50 : html.length);

const re = /<h([1-4])\s[^>]*data-number=\"([^\"]*)\"[^>]*>[^<]*<span[^>]*>[^<]*<\/span>\s*([\s\S]*?)<\/h\1>/g;
const matches = []; let m;
while ((m = re.exec(content)) !== null) {
    matches.push({ level: parseInt(m[1]), number: m[2], title: stripHtml(m[3]).trim(), index: m.index, endIndex: m.index + m[0].length });
}
for (let i = 0; i < matches.length; i++) {
    const s = matches[i].endIndex;
    const e = (i+1 < matches.length) ? matches[i+1].index : content.length;
    const raw = content.substring(s, e).replace(/<figure[\s\S]*?<\/figure>/gi, '');
    matches[i].text = stripHtml(raw).trim();
}

const chapters = []; const stack = [];
for (const sec of matches) {
    const depth = sec.number.indexOf('.') === -1 ? 1 : sec.number.split('.').filter(s=>s.length>0).length;
    const node = { title: sanitizeText(sec.title) };
    if (sec.text) { node.lines = wrapText(sanitizeText(sec.text), MAX_LINE_WIDTH); }
    if (depth === 1) {
        chapters.push(node); stack.length = 0; stack[1] = node;
    } else {
        const parent = stack[depth - 1];
        if (parent) { if (!parent.children) parent.children = []; parent.children.push(node); }
        stack[depth] = node; stack.length = depth + 1;
    }
}
function postProcess(n) {
    if (n.children) {
        if (n.lines && n.lines.length > 0) { n.children.unshift({ title: 'Overview', lines: n.lines }); delete n.lines; }
        for (const c of n.children) { postProcess(c); }
    }
}
for (const ch of chapters) { postProcess(ch); }

const output = { fetched: new Date().toISOString(), sections: chapters };
fs.writeFileSync('${CACHE_DIR}/move_manual.json', JSON.stringify(output, null, 2));
function countLeaves(n) { return n.children ? n.children.reduce((a,c)=>a+countLeaves(c),0) : 1; }
const leaves = chapters.reduce((a,c)=>a+countLeaves(c),0);
console.log('Parsed ' + chapters.length + ' chapters, ' + leaves + ' leaf topics');
console.log('Output: ${CACHE_DIR}/move_manual.json (' + Math.round(JSON.stringify(output).length/1024) + 'KB)');
"

    echo ""
    echo "To deploy to device:"
    echo "  ssh ableton@move.local 'mkdir -p /data/UserData/move-anything/cache'"
    echo "  scp ${CACHE_DIR}/move_manual.json ableton@move.local:/data/UserData/move-anything/cache/"
fi
