/*
 * Shadow UI — the NOTICE screen: a title, a few lines, and a click that
 * dismisses.
 *
 * This file was shadow_ui_store.mjs and drew the Module Store's category list,
 * module list, detail card, loading frames and result card. The store's
 * browsing and install views went when the on-device install path did; the
 * pointer screens that replaced them have now gone to the Connect view, which
 * can give a real address instead of a sentence about one.
 *
 * What is left is the result card, doing the one job it had underneath: say
 * something and wait. It is renamed for that job rather than kept under a
 * shop's name, and it has exactly one caller — the boot-time Schwung Repair
 * banner, which has a real sentence to say and nothing to point at.
 *
 * (`buildReleaseNoteLines` went with the rename. It wrapped a module's release
 * notes for the detail card, which had already been deleted, so it was dead
 * code kept alive by a file name that still described the old shape.)
 */
import { ctx } from './shadow_ui_ctx.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter
} from '/data/UserData/schwung/shared/menu_layout.mjs';

export function drawNotice() {
    const { noticeTitle, noticeMessage } = ctx;

    clear_screen();
    drawHeader(noticeTitle || 'Schwung');

    const msg = noticeMessage || 'Done';
    /* Multi-line support: callers set the message with embedded "\n" and each
     * line is stacked. startY places the first body line directly under the
     * header bar with no leading blank-line gap. */
    const lines = String(msg).split('\n');
    const lineHeight = 10;
    const startY = 18;
    for (let i = 0; i < lines.length; i++) {
        print(2, startY + i * lineHeight, lines[i], 1);
    }

    drawFooter('Press to continue');
}
