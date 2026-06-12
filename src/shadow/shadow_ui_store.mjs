/*
 * Shadow UI - Module Store views (categories, list, detail, loading, result).
 *
 * Extracted from shadow_ui.js to allow forks to modify store
 * presentation without touching core.
 */
import { ctx } from './shadow_ui_ctx.mjs';
import {
    SCREEN_WIDTH,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT,
    LIST_LABEL_X, LIST_VALUE_X,
    FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/schwung/shared/chain_ui_views.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList
} from '/data/UserData/schwung/shared/menu_layout.mjs';
import {
    announce
} from '/data/UserData/schwung/shared/screen_reader.mjs';

/* ---- Helpers ------------------------------------------------------------ */

function buildReleaseNoteLines(notesText) {
    const { wrapText } = ctx;
    const lines = [];
    const noteLines = notesText.split('\n');
    for (const line of noteLines) {
        if (line.trim() === '') {
            lines.push('');
        } else {
            const cleaned = line.trim()
                .replace(/^#+\s*/, '')
                .replace(/\*\*/g, '')
                .replace(/\*/g, '');
            const wrapped = wrapText(cleaned, 20);
            lines.push(...wrapped);
        }
    }
    return lines;
}

/* ---- Draw --------------------------------------------------------------- */

export function drawStorePickerResult() {
    const { storePickerResultTitle, storePickerMessage } = ctx;

    clear_screen();
    drawHeader(storePickerResultTitle || 'Module Store');

    const msg = storePickerMessage || 'Done';
    /* Multi-line support: callers may set the message with embedded "\n"
     * (e.g. the Update Schwung pointer) — render each line stacked. */
    const lines = String(msg).split('\n');
    const lineHeight = 10;
    /* startY chosen to place the first body line directly under the
     * header bar with no leading blank-line gap. */
    const startY = 18;
    for (let i = 0; i < lines.length; i++) {
        print(2, startY + i * lineHeight, lines[i], 1);
    }

    drawFooter('Press to continue');
}

