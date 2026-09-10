/*
 * e16_view.mjs -- the PARAMETERS view: two authored grid pages under sixteen
 * encoders.
 *
 * PURE AND INJECTED, like every other e16_* module on this branch. It takes a
 * page plan (whatever `planPages()` produced) plus two lookup functions and
 * returns a view model; it never reads a param, never sends a byte, and never
 * writes a value. tests/host drives all of it with no device and no shim.
 *
 * ------------------------------------------------------------------------
 * WHY TWO AUTHORED PAGES AND NOT ONE RE-PLANNED 16-CELL PAGE
 *
 * The obvious thing to do with sixteen encoders is to re-chunk the module's
 * params sixteen at a time. docs/PARAM_PAGES.md forbids exactly that: a
 * module's page groupings are AUTHORED, and the planner's `paginate` flag
 * exists because "eight" is the number of physical knobs on MOVE, not a
 * property of the contract. A deliberate six-param Filter page re-cut into a
 * sixteen-cell page carrying half of Filter and half of Envelope is not a
 * denser view of the same module, it is a different module's layout.
 *
 * So: top 2x4 is page N, bottom 2x4 is page N+1, both untouched. A page with
 * six keys leaves cells 6 and 7 EMPTY rather than pulling the next page's
 * first two knobs forward -- see buildView(). That is the same rule stated at
 * the cell level, and it is the one a "helpful" compaction would break first.
 *
 * WHY A TURN GOES BACK THROUGH THE CONTROLLER
 *
 * applyTurn() calls the grid's own `onKnobTurn`. It is load-bearing rather
 * than tidy: enum quantization, read-only refusal, momentary latching, the
 * fine-adjust step and `visible_if` gating all live behind that one call, and
 * every one of them would have to be re-derived -- and would drift -- in a
 * second value-application path. This module therefore knows nothing about any
 * module. `readOnly` is reported in the cell for DRAWING only; the refusal
 * itself is the controller's, and must stay there.
 * ------------------------------------------------------------------------
 */

import { KNOBS_PER_PAGE, PAGE_KNOBS, pageSlotKeys } from "./param_pages/page_plan.mjs";

/* Sixteen encoders, 4x4, row-major: enc 0-3 is the top row. Encoders 0-7 are
 * the top half (page N), 8-15 the bottom (page N+1). The halves are two rows
 * each because that is what a 2x4 authored page IS -- any other split would
 * put one page's knobs on both sides of the screen. */
export const ENCODERS = 16;
export const COLS = 4;
export const HALVES = 2;

/* Screen geometry, 128x64. A half is 32 px: an 8 px header bar and two 12 px
 * cell rows (8 + 12 + 12 = 32). Exported because the encoder-to-cell mapping
 * and the drawn layout have to be THE SAME FACT -- a surface where knob 5 is
 * under the cell knob 6 draws in is not a bug you find by reading code.
 *
 * `HEADER_BAR_H` rather than `HEADER_H`: that name is one of twelve list-chrome
 * constants `tests/host/test_list_behavior.sh` requires to have EXACTLY ONE
 * definition in src/, because a forked copy of the list geometry once left the
 * whole movy re-skin inert on the device while the test stayed green. This is a
 * different screen with its own geometry, so it takes its own name rather than
 * a second definition of theirs. */
export const WIDTH = 128;
export const HALF_H = 32;
export const HEADER_BAR_H = 8;
export const CELL_W = WIDTH / COLS;             /* 32 */
export const CELL_H = (HALF_H - HEADER_BAR_H) / 2;  /* 12 */

/* The header names its page in the LEFT half of the bar and leaves the right
 * half for the page position. 64 px of font4x5 is about fifteen characters,
 * which is most page names; anything longer is truncated with a trailing dot
 * rather than allowed to run under the position text. */
export const HEADER_TEXT_W = WIDTH / 2;

/* Ring colours. Two, not a palette: the ring's JOB is to show a value, and a
 * per-cell hue would encode something this view does not know. The dim pair is
 * for a read-only cell, so a readout is visibly not a control -- the same
 * distinction `access: "read"` draws as a dotted stroke on the knob grid. */
export const RING_RGB = { r: 0, g: 40, b: 40 };
export const RING_RGB_READONLY = { r: 0, g: 8, b: 8 };

/* The E16 ring position is 14-bit. */
export const RING_MAX = 16383;

/* Protocol cap on one relative-CC turn (docs/E16_REMOTE.md: relative with
 * acceleration, decoded by e16_input across the full 7-bit two's complement
 * range). A cap here is belt and braces against a garbled CC turning into a
 * thousand writes on the param channel. */
export const MAX_TICKS_PER_TURN = 63;

/**
 * Which half of the screen an encoder belongs to, and which slot of that
 * half's page it drives. The single definition of the mapping; everything
 * else -- the rects, the cells, the turn routing -- is derived from it, so
 * "the mapping matches the drawn layout" is true by construction.
 */
export function encHalf(enc) { return Math.floor(enc / KNOBS_PER_PAGE); }
export function encSlot(enc) { return enc % KNOBS_PER_PAGE; }

/** The rect an encoder's cell is drawn in. */
export function cellRect(enc) {
    const half = encHalf(enc);
    const slot = encSlot(enc);
    return {
        x: (slot % COLS) * CELL_W,
        y: half * HALF_H + HEADER_BAR_H + Math.floor(slot / COLS) * CELL_H,
        w: CELL_W,
        h: CELL_H,
    };
}

/**
 * The eight keys of a page, or eight nulls.
 *
 * `pageSlotKeys` is the planner's own exported answer to this question and is
 * used for the ordinary case. It tests `kind === PAGE_KNOBS` strictly, which
 * predates `as_page`: a module-owned canvas page carries real keys and its own
 * kind, and the controller asks `pageHasKnobs` -- "does it have keys" -- for
 * exactly that reason (CLAUDE.md). Blanking such a page here would be a
 * surface that goes dark on the one page a module authored deliberately, so
 * the canvas case is handled beside the delegation rather than by restating
 * the whole function.
 */
function slotsOf(page) {
    if (page && page.kind !== PAGE_KNOBS && page.canvas &&
        Array.isArray(page.keys) && page.keys.length) {
        const out = new Array(KNOBS_PER_PAGE).fill(null);
        for (let i = 0; i < Math.min(page.keys.length, KNOBS_PER_PAGE); i++) {
            out[i] = page.keys[i];
        }
        return out;
    }
    return pageSlotKeys(page);
}

function headerOf(page, index, count) {
    if (!page) return null;
    return { name: page.name || "", index, count };
}

/**
 * Build the view model for the page pair starting at `pageIndex`.
 *
 * @param {Array}  pages       planPages().pages
 * @param {number} pageIndex   index of the TOP page (N); N+1 fills the bottom
 * @param {object} [io]
 * @param {function} [io.metaOf]   key -> param meta (param_meta shape)
 * @param {function} [io.valueOf]  key -> current value
 * @returns {{cells: Array, headers: Array, pageIndex: number, pageCount: number}}
 *
 * `cells` is always ENCODERS long and indexed BY ENCODER, so cells[e] is what
 * encoder e drives and null means that encoder does nothing. A shorter array
 * indexed by "occupied cell" would make every consumer re-derive the mapping.
 */
export function buildView(pages, pageIndex, io) {
    const o = io || {};
    const metaOf = o.metaOf || (() => null);
    const valueOf = o.valueOf || (() => undefined);
    const list = Array.isArray(pages) ? pages : [];
    const idx = pageIndex | 0;

    const cells = new Array(ENCODERS).fill(null);
    const headers = new Array(HALVES).fill(null);

    for (let half = 0; half < HALVES; half++) {
        const p = list[idx + half] || null;
        /*
         * A component with a single page leaves the bottom half DARK. There is
         * deliberately no `|| list[idx + half - 1]` and no wrap to page 0: a
         * second copy of page N under the lower eight encoders would make the
         * same parameter reachable from two knobs, and wrapping would put the
         * module's LAST page under its first. Absent is absent.
         */
        if (!p) continue;
        headers[half] = headerOf(p, idx + half, list.length);
        const keys = slotsOf(p);
        for (let slot = 0; slot < KNOBS_PER_PAGE; slot++) {
            const key = keys[slot];
            if (!key) continue;   /* a 6-key page: cells 6 and 7 stay null */
            const meta = metaOf(key) || {};
            const min = typeof meta.min === "number" ? meta.min : 0;
            const max = typeof meta.max === "number" ? meta.max : 1;
            cells[half * KNOBS_PER_PAGE + slot] = {
                enc: half * KNOBS_PER_PAGE + slot,
                half,
                slot,
                pageIndex: idx + half,
                key,
                /* The authored short label lives on the PAGE, not the
                 * meta: page_plan collects { key: short_name } into
                 * page.shortNames and render_page_movy reads it there.
                 * getOrGuess never carries it, so meta.short_name is
                 * essentially always undefined and this fell through to the
                 * raw parameter id -- which at 4px per character is a
                 * fragment, and four columns of fragments read as noise. */
                label: (p.shortNames && p.shortNames[key]) ||
                       meta.short_name || meta.label || key,
                value: valueOf(key),
                min,
                max,
                /* Bipolar is READ FROM THE RANGE, exactly as render_page_movy
                 * reads it (min < 0). There is no meta.bipolar field, and
                 * inventing one here would be a second answer to a question
                 * the grid already answers. */
                bipolar: min < 0,
                readOnly: !!meta.readOnly,
            };
        }
    }

    return { cells, headers, pageIndex: idx, pageCount: list.length };
}

/* 0..RING_MAX for a cell's current value, clamped. A non-numeric value (an
 * enum reported as a name, a param not yet read) parks the ring at zero rather
 * than at NaN, which the protocol would truncate into a random position. */
export function ringAmount(cell) {
    if (!cell) return 0;
    const v = Number(cell.value);
    if (!isFinite(v)) return 0;
    const span = cell.max - cell.min;
    if (!(span > 0)) return 0;
    const t = (v - cell.min) / span;
    return Math.max(0, Math.min(RING_MAX, Math.round(t * RING_MAX)));
}

/** One ring descriptor for `ringMsg`, or null for an empty cell. */
export function ringFor(view, enc) {
    const cell = view && view.cells ? view.cells[enc] : null;
    if (!cell) return null;
    const rgb = cell.readOnly ? RING_RGB_READONLY : RING_RGB;
    return { enc, r: rgb.r, g: rgb.g, b: rgb.b,
             amount: ringAmount(cell), bipolar: cell.bipolar };
}

/** Every occupied cell's ring, for the one full refresh that follows a nav. */
export function ringsFor(view) {
    const out = [];
    for (let e = 0; e < ENCODERS; e++) {
        const r = ringFor(view, e);
        if (r) out.push(r);
    }
    return out;
}

function clip(ctx, text, w) {
    let s = String(text === undefined || text === null ? "" : text);
    if (ctx.textWidth(s) <= w) return s;
    while (s.length && ctx.textWidth(s + ".") > w) s = s.slice(0, -1);
    return s + ".";
}

/**
 * Draw the view into an e16 canvas (or anything with the param_pages draw
 * context: fillRect / print / textWidth / clear).
 *
 * Two headers, one per half, each naming ITS page -- a single header could not
 * name two pages honestly, which is also why this renders pixels instead of
 * using the LABELS message (docs/E16_REMOTE.md: one 16-char title for the
 * whole screen).
 *
 * Nothing is drawn into a half whose page is absent. That is the dark-bottom
 * rule made visible: a test can assert the lower 512 bytes of the framebuffer
 * are zero, which no amount of cell bookkeeping can fake.
 */
export function renderView(ctx, view) {
    ctx.clear();
    for (let half = 0; half < HALVES; half++) {
        const h = view.headers[half];
        if (!h) continue;
        const y = half * HALF_H;
        /* Inverted header bar, so the eye finds the two pages before it reads
         * either -- the split is the whole point of this screen. */
        ctx.fillRect(0, y, WIDTH, HEADER_BAR_H - 1, 1);
        ctx.print(1, y + 1, clip(ctx, h.name, HEADER_TEXT_W - 2), 0);
        const pos = `${h.index + 1}/${h.count}`;
        ctx.print(WIDTH - 1 - ctx.textWidth(pos), y + 1, pos, 0);
    }
    for (let e = 0; e < ENCODERS; e++) {
        const cell = view.cells[e];
        if (!cell) continue;
        const r = cellRect(e);
        ctx.print(r.x + 1, r.y + 1, clip(ctx, cell.label, r.w - 2), 1);
        const v = cell.value === undefined || cell.value === null ? "" : String(cell.value);
        if (v !== "") ctx.print(r.x + 1, r.y + 7, clip(ctx, v, r.w - 2), 1);
    }
}

/* ---------------------------------------------------------------------------
 * THE MAP VIEW -- what Shift shows.
 *
 * The model is `buildMap()` in e16_map.mjs and it is not restated here: this
 * function only turns its sixteen cells into pixels. Same division as the
 * parameters view, and for the same reason -- the layout rules (holes are not
 * destinations, twelve cells with pagination past that) have to be runnable in
 * tests/host without a renderer, and the renderer has to be checkable in
 * PIXELS without re-deriving the layout.
 *
 * Geometry is the 4x4 the device IS: 32 x 16 per cell, so a cell sits exactly
 * under its own encoder. There is no header bar -- unlike the parameters view
 * there is nothing to name that the sixteen cells do not already say, and 8 px
 * of chrome would cost the bottom row a readable second line.
 * ------------------------------------------------------------------------- */
export const MAP_ROWS = 4;
export const MAP_CELL_W = WIDTH / COLS;          /* 32 */
export const MAP_CELL_H = (HALF_H * HALVES) / MAP_ROWS;  /* 16 */

/** The rect a map cell is drawn in. Row-major, so cell i is under encoder i. */
export function mapCellRect(i) {
    return {
        x: (i % COLS) * MAP_CELL_W,
        y: Math.floor(i / COLS) * MAP_CELL_H,
        w: MAP_CELL_W,
        h: MAP_CELL_H,
    };
}

/**
 * Draw a map.
 *
 * @param {object} ctx  an e16 canvas (or any param_pages draw context)
 * @param {object} map  buildMap()'s return: { cells, pageCount }
 * @param {object} [opts]
 * @param {number} [opts.page]  which map page is shown, for the indicator
 *
 * A cell is drawn ONLY where the model put one. An empty lower cell leaves
 * blank pixels rather than an outline, because an outline is an affordance and
 * `e16_map.mjs` is explicit that a hole is not a destination -- drawing the box
 * anyway would offer a button that does nothing, which is the exact thing the
 * compaction rule exists to prevent, reintroduced one layer down.
 *
 * Each component cell carries TWO lines: the module's name and its position id
 * ("fx2", "midi_fx1", "bus1"). The id is not decoration -- two Freeverbs in one
 * slot are indistinguishable by name, and the id is what the press addresses.
 */
export function renderMap(ctx, map, opts) {
    const o = opts || {};
    const page = o.page | 0;
    const cells = (map && map.cells) || [];
    const pageCount = (map && map.pageCount) || 1;
    ctx.clear();

    for (let i = 0; i < ENCODERS; i++) {
        const cell = cells[i];
        if (!cell) continue;
        const r = mapCellRect(i);
        const isSlot = cell.kind === "slot";
        /* The current slot is INVERTED rather than merely outlined: this is the
         * one fact the eye has to find before it reads anything else, since
         * every lower cell means something different depending on it. */
        const fill = isSlot && cell.current;
        if (fill) ctx.fillRect(r.x, r.y, r.w - 1, r.h - 1, 1);
        const ink = fill ? 0 : 1;
        if (isSlot && !fill) {
            ctx.drawLine(r.x, r.y, r.x + r.w - 2, r.y, 1);
            ctx.drawLine(r.x, r.y + r.h - 2, r.x + r.w - 2, r.y + r.h - 2, 1);
            ctx.drawLine(r.x, r.y, r.x, r.y + r.h - 2, 1);
            ctx.drawLine(r.x + r.w - 2, r.y, r.x + r.w - 2, r.y + r.h - 2, 1);
        }
        ctx.print(r.x + 2, r.y + 2, clip(ctx, cell.label, r.w - 4), ink);
        if (!isSlot) {
            ctx.print(r.x + 2, r.y + 9, clip(ctx, cell.component, r.w - 4), ink);
        } else if (fill && pageCount > 1) {
            /* The indicator lives on the CURRENT SLOT cell because that is the
             * cell whose list is being paged -- a page number floating in a
             * corner would belong to nothing on a screen that is otherwise
             * entirely made of cells. */
            const pos = `${page + 1}/${pageCount}`;
            ctx.print(r.x + r.w - 2 - ctx.textWidth(pos), r.y + 9, pos, ink);
        }
    }
}

/**
 * Step the parameter view's page pair.
 *
 * BY TWO, not by one. The screen shows N in the top half and N+1 in the
 * bottom, so a single-page step would put the page you were just reading under
 * the other eight encoders -- half the surface would appear not to have moved
 * while every one of its knobs quietly changed which parameter it drives.
 *
 * A step that would run past the last page is REFUSED rather than clamped to
 * `pageCount - 1`: clamping changes the parity of the pair, so a six-page
 * component paged 0/1 -> 2/3 -> 4/5 would come back 5/-, i.e. one page shown
 * twice in a row and one never reachable as a top half. Refusing keeps the
 * pairing an invariant of the whole traversal rather than of one step.
 */
export function pageStep(pageIndex, ticks, pageCount) {
    let i = Math.max(0, pageIndex | 0);
    const last = Math.max(0, (pageCount | 0) - 1);
    if (!ticks) return Math.min(i, last);
    const dir = ticks > 0 ? HALVES : -HALVES;
    const n = Math.min(Math.abs(ticks | 0), MAX_TICKS_PER_TURN);
    for (let k = 0; k < n; k++) {
        const next = i + dir;
        if (next < 0 || next > last) break;
        i = next;
    }
    return i;
}

/**
 * Route an encoder turn to the grid's own knob-turn path.
 *
 * `ctl` is a page controller (createController) or anything with the same
 * three members. Injected rather than imported so tests can record the calls,
 * and so this file has no way to write a parameter itself.
 *
 * THE PAGE SWAP IS NOT OPTIONAL. The controller has ONE current page and
 * `onKnobTurn(slot)` resolves the key against it -- so a turn on the bottom
 * half must move the controller to page N+1 first or it writes to page N's
 * key of the same slot number. Both are small in-range integers and both
 * exist, so the wrong one is a knob that silently edits a different parameter,
 * with nothing logged. `remember: false` because section memory is the jog's
 * notion of where you were, and this is not a navigation.
 *
 * @returns {{key: string, enc: number, ticks: number}|null} what moved, so the
 *          caller can mark that ONE ring dirty. null for an empty encoder.
 */
export function applyTurn(view, ctl, enc, ticks, nowMs) {
    const cell = view && view.cells ? view.cells[enc] : null;
    if (!cell || !ctl || !ticks) return null;
    if (ctl.pageIndex !== cell.pageIndex && ctl.goToPage) {
        ctl.goToPage(cell.pageIndex, { remember: false });
    }
    const dir = ticks > 0 ? 1 : -1;
    const n = Math.min(Math.abs(ticks), MAX_TICKS_PER_TURN);
    /* One call per detent. The E16's relative CC already carries the
     * acceleration, and the grid's knob engine applies its own on top from the
     * timestamps -- so N detents must arrive as N turns, not as one turn with a
     * multiplier the engine has no parameter for. */
    for (let i = 0; i < n; i++) ctl.onKnobTurn(cell.slot, dir, nowMs, { fine: false });
    return { key: cell.key, enc, ticks: n * dir };
}

/**
 * Route an encoder PUSH to the grid's own click path.
 *
 * Same page swap and the same reason as applyTurn -- `onClick(slot)` resolves
 * the slot against the controller's current page, so a push on the bottom half
 * would otherwise click page N's cell. Everything a click MEANS (a two-option
 * flip, a trigger firing, a door opening) lives in the controller, and must
 * stay there: this file has no second answer to any of it.
 *
 * @returns {{key: string, enc: number}|null} what was clicked, or null for an
 *          empty encoder.
 */
export function applyClick(view, ctl, enc) {
    const cell = view && view.cells ? view.cells[enc] : null;
    if (!cell || !ctl || typeof ctl.onClick !== "function") return null;
    if (ctl.pageIndex !== cell.pageIndex && ctl.goToPage) {
        ctl.goToPage(cell.pageIndex, { remember: false });
    }
    ctl.onClick(cell.slot);
    return { key: cell.key, enc };
}

/*
 * A layout probe, for reading the framebuffer convention off the device.
 *
 * "The screen is garbled" cannot distinguish a wrong bit direction from a
 * wrong page order from a wrong stride -- every one of them produces
 * structured nonsense, and describing structured nonsense over a chat window
 * is unreliable. Each of these draws ONE unambiguous thing, so the question
 * becomes "is there a line along the top?" rather than "what does it look
 * like?".
 *
 *   0  a single lit row at y=0        -> top row. If it appears at the BOTTOM
 *                                       of the first band, bit order in the
 *                                       page is inverted; if 8 rows down, the
 *                                       page stride is wrong.
 *   1  a single lit column at x=0     -> left edge. Diagonal or repeated means
 *                                       the row stride is wrong.
 *   2  page 0 filled solid            -> the top 8 rows only. Anywhere else and
 *                                       the page order is not what we assume.
 *   3  a 16px box at the origin       -> corner, orientation and scale at once.
 *
 * Kept in the view module rather than a test file because it has to run on the
 * device, through the same canvas and the same send path as a real frame --
 * a probe that takes a different route measures the route, not the format.
 */
export function drawTestPattern(ctx, which) {
    ctx.clear();
    const n = (which | 0) % 6;
    if (n === 0) ctx.fillRect(0, 0, 128, 1, 1);
    else if (n === 1) ctx.fillRect(0, 0, 1, 64, 1);
    else if (n === 2) ctx.fillRect(0, 0, 128, 8, 1);
    else if (n === 3) ctx.fillRect(0, 0, 16, 16, 1);
    /*
     * 4 marks the FAR END of the buffer: the bottom 8 rows are page 7, bytes
     * 896-1023, the last thing in the message. The header (page 0, bytes
     * 0-127) renders correctly while everything below it does not, and that is
     * exactly the shape of a device that accepts the front of the message and
     * not the back. If this band appears, the whole buffer lands and the fault
     * is our content; if it does not, the device is truncating and no amount
     * of drawing will fix it.
     */
    else if (n === 4) ctx.fillRect(0, 56, 128, 8, 1);
    /*
     * 5 lights EVERY pixel the buffer can address.
     *
     * Content was observed SURVIVING an off/on cycle, which a full frame of
     * zeros cannot allow: either the device ORs rather than replaces, or our
     * 1024 bytes do not span the whole panel. Filling everything separates
     * them by inspection -- if the entire screen goes white, we address all of
     * it and the persistence is a clear/replace question; if only part does,
     * that part IS what 1024 bytes covers and the rest is a geometry we have
     * not accounted for.
     */
    else ctx.fillRect(0, 0, 128, 64, 1);
}
