/*
 * e16_diff.mjs -- decides how to describe the difference between two
 * SSD1306 page/column framebuffers: as zero, one, or a few small SCANLINE/
 * RECTANGLE regions, or as "give up, send the whole FRAMEBUFFER".
 *
 * A SINGLE BOUNDING BOX OVER THE WHOLE DIFF IS THE WRONG SHAPE. Two
 * far-apart changes -- e.g. the map cursor moving from a top-left cell to a
 * bottom-right one, an entirely ordinary navigation -- produce one box
 * spanning nearly the whole screen even though the actual changed pixels
 * are a couple of percent of it. Naively that either sends one needlessly
 * huge RECTANGLE, or trips a whole-screen-area threshold and falls back to
 * FRAMEBUFFER -- defeating the point in exactly the case (fast navigation)
 * partial updates are for.
 *
 * So this clusters by ROW RUN instead of one global box:
 *   1. which of the 64 rows differ at all
 *   2. group contiguous differing rows into runs
 *   3. for EACH run, take the x-bounds from only the pixels that differ
 *      WITHIN that run's rows -- this is what keeps a run over one cell
 *      from being dragged wide by a change in a totally different cell;
 *      two far-apart changes land in two different runs with two different,
 *      each-small x-ranges.
 *
 * It does not fully solve two changes in the SAME row-band but far apart in
 * x (e.g. top-left and top-right cells): those collapse into one run whose
 * x-range spans both. That run is still small in AREA though (one row-band
 * tall), so it costs little on the wire even in that case; true 2D
 * connected-component clustering would be more optimal but is real added
 * complexity (flood-fill/union-find, multi-rect merging) for a screen this
 * small, where the wire-cost difference is modest. Revisit only if hardware
 * timing shows row-runs aren't good enough.
 */
import { readPixel, WIDTH, HEIGHT } from "./e16_canvas.mjs";

export const MAX_REGIONS = 4;
export const FULL_REPAINT_THRESHOLD = 0.4;

function rowXBounds(prev, next, y) {
    let x0 = -1, x1 = -1;
    for (let x = 0; x < WIDTH; x++) {
        if (readPixel(prev, x, y) !== readPixel(next, x, y)) {
            if (x0 === -1) x0 = x;
            x1 = x;
        }
    }
    return x0 === -1 ? null : { x0, x1 };
}

function regionForRun(prev, next, yStart, yEnd) {
    let x0 = WIDTH, x1 = -1;
    for (let y = yStart; y <= yEnd; y++) {
        const b = rowXBounds(prev, next, y);
        if (!b) continue;
        if (b.x0 < x0) x0 = b.x0;
        if (b.x1 > x1) x1 = b.x1;
    }
    const y = yStart, h = yEnd - yStart + 1, x = x0, w = x1 - x0 + 1;
    if (h === 1 && w === WIDTH) return { kind: "scanline", y };
    return { kind: "rect", x, y, w, h };
}

/**
 * @param {Uint8Array|null} prev  what we believe the device shows, or null
 *        if unknown (forces a full repaint -- there is nothing to diff
 *        against).
 * @param {Uint8Array} next       the freshly rendered 1024-byte buffer.
 * @param {object} [opts]
 * @param {number} [opts.maxRegions]            default MAX_REGIONS
 * @param {number} [opts.fullRepaintThreshold]  default FULL_REPAINT_THRESHOLD
 * @returns {{kind:"none"}|{kind:"full"}|{kind:"regions",regions:Array}}
 */
export function diffFramebuffers(prev, next, opts) {
    const o = opts || {};
    const maxRegions = o.maxRegions === undefined ? MAX_REGIONS : o.maxRegions;
    const threshold = o.fullRepaintThreshold === undefined
        ? FULL_REPAINT_THRESHOLD : o.fullRepaintThreshold;
    const maxArea = threshold * WIDTH * HEIGHT;

    if (!prev || prev.length !== 1024 || next.length !== 1024) return { kind: "full" };

    const runs = [];
    let runStart = -1;
    for (let y = 0; y < HEIGHT; y++) {
        const differs = rowXBounds(prev, next, y) !== null;
        if (differs && runStart === -1) runStart = y;
        if (!differs && runStart !== -1) { runs.push([runStart, y - 1]); runStart = -1; }
    }
    if (runStart !== -1) runs.push([runStart, HEIGHT - 1]);

    if (runs.length === 0) return { kind: "none" };
    if (runs.length > maxRegions) return { kind: "full" };

    const regions = [];
    for (const [yStart, yEnd] of runs) {
        const region = regionForRun(prev, next, yStart, yEnd);
        const area = region.kind === "scanline" ? WIDTH : region.w * region.h;
        if (area > maxArea) return { kind: "full" };
        regions.push(region);
    }
    return { kind: "regions", regions };
}
