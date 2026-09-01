/*
 * The snapshot / recall toast.
 *
 * Now just a call into overlay_card.mjs — the frame, the band and the
 * measured centring all live there, shared with every other overlay on the
 * device. This file is what remains that is specific to the gesture: which
 * words go in the band, which go under it, and the fact that the toast has to
 * hand its rect to the shim when Move's screen is the one being drawn over.
 *
 * It began as its own hand-rolled box, which is how it acquired the two bugs
 * that made this consolidation worth doing: a fixed-vs-content width that made
 * the box change shape between messages, and baseline arithmetic on a `print`
 * whose y is the glyph TOP.
 */

import { drawOverlayCard } from './overlay_card.mjs';

/*
 * "Snapshot" is the band; the outcome is the value on its right. That is the
 * same shape every other card uses — the noun on the left, what happened to it
 * on the right — and it puts the word that differs between the two gestures
 * where the eye already goes for a value.
 */
export function drawSnapshotToast(lines) {
    const rows = (lines || []).filter(Boolean);
    return drawOverlayCard(null, {
        title: "Snapshot",
        titleRight: rows[0] || "",
        lines: rows.slice(1),
    });
}
