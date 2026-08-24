/**
 * page_controller.mjs — the interaction model for a knob page.
 *
 * This is the part that would normally live inside shadow_ui.js as a few
 * hundred lines of view state, and therefore be untestable without a Move. It
 * is here instead, pure, with every device call injected:
 *
 *   getParam(fullKey)        -> string|null
 *   setParam(fullKey, value) -> void
 *   announce(text)           -> void          (optional)
 *   isModulated(fullKey)     -> boolean       (optional)
 *   now()                    -> ms            (optional, injectable clock)
 *
 * What is left for the real binding is genuinely thin: route MIDI to the
 * handlers below, call `tick()` once a frame, and call `render()`. Everything
 * with a decision in it — knob feel, staggered reads, when to rebuild, what to
 * announce — is testable here.
 *
 * Two behaviours carry most of the risk and both are pinned by tests:
 *
 *   Staggered reads. Eight live values per page is eight IPC round trips.
 *   Movy measured bulk refresh blocking ~186 ms per cycle. This reads ONE param
 *   per tick, cycling the current page.
 *
 *   Write-back suppression. A read issued before a knob turn lands after it and
 *   would drag the value backwards. Reads are ignored for a key while it is
 *   being turned and for a short settling window afterwards.
 */

import { planPages, PAGE_KNOBS, PAGE_MENU, PAGE_PRESET, PAGE_ITEMS,
         buildTrailingPages, makeClaimer } from "./page_plan.mjs";
import { resolveChildKey } from "./child_key.mjs";
import { buildMetaIndex, inferFromValue, isTurnable, enumIndexOf, KIND_ENUM, KIND_OPAQUE } from "./param_meta.mjs";
import { renderPage, renderPicker, renderHint, LAYOUT_DIAL } from "./render_page.mjs";
import { renderPageMovy, drawFooter, drawHeader as drawHeaderMovy, drawBankBar,
         drawBrackets, drawPresetBody, displayValue, RULE_Y, LAYOUT_MOVY,
         MENU_LIST_X, MENU_LIST_Y, MENU_LIST_W } from "./render_page_movy.mjs";
import { resolveViz } from "./viz.mjs";
import { drawMenuList } from "../menu_layout.mjs";

export { LAYOUT_MOVY };

/**
 * The knob page as FIVE ROWS instead of eight cells.
 *
 * A layout, not an engine. Everything a page is — which params are on it
 * (page_plan), what type and range they have (param_meta), how a value reads
 * (the exported displayValue), how a detent moves it (knob_engine, reached
 * through this controller's own onKnobTurn), what is spoken (announce_page) and
 * the chrome around it (render_page_movy) — is the same code under both. The
 * ONLY difference is pixel arrangement, which is the one difference the design
 * calls irreducible.
 *
 * That is the whole reason it lives here rather than in a renderer beside
 * render_page_movy.mjs. The five-row list already exists in this file and
 * already draws three page kinds through drawPageChromeList; a knobs page shown
 * as a list is that same list fed the page's PARAMS instead of its entries. A
 * mode flag threaded through render_page_movy's 1600 lines would be the `geom`
 * all-or-nothing trap in another costume.
 *
 * NOT yet selected by `param_view` — wiring that seam is a separate act with the
 * whole fleet behind it (see §4.1 of the design). Reachable today only through
 * setLayout(LAYOUT_LIST).
 */
export const LAYOUT_LIST = "list";
import { step, stepLevel, reanchor, firstGrid, jumpIndex, groupIndex } from "./page_nav.mjs";

/*
 * Menu page geometry.
 *
 * The list keeps FIVE rows, the same as every other list on this screen, in
 * both the inert and the entered state — so entering a menu highlights a row
 * rather than reflowing the page. The bracket frame is therefore drawn OUTSIDE
 * the list, not around a shrunken one:
 *
 *   row 8        top bracket arms
 *   rows 9..53   the five list rows (fills are y-1 .. y+7 from y=10,19,28,37,46)
 *   row 54       bottom bracket arms
 *
 * and horizontally the frame sits at x=2..125 while the list text starts at
 * x=10 and its right-aligned values end at x=118, so nothing collides and
 * nothing touches the screen edge.
 */
/* The rect itself now lives with the rest of the movy chrome, in
 * render_page_movy.mjs beside HEADER_H and RULE_Y — its consumers are
 * RENDERERS, and menu_layout.mjs was importing this 2300-line controller (and
 * the whole engine behind it) to obtain two integers, which is real parse cost
 * on the device's QuickJS. Re-exported under the same names so every existing
 * importer is untouched; still one definition. */
export { MENU_LIST_X, MENU_LIST_Y, MENU_LIST_W } from "./render_page_movy.mjs";
/*
 * The frame lives in the list margin, one pixel clear of the dividers.
 *
 * Horizontally: side arms at x=4 and x=123, inside the screen edge and outside
 * the text (starts x=10) and the right-aligned values (end x=118).
 *
 * Vertically there looked to be no room — five rows at a 9px stride is 45 of
 * the 47 rows between the bank bar and the footer rule. But the list only
 * FILLS a row when it is selected, and an inert menu has nothing selected, so
 * the only occupied rows are the glyphs themselves:
 *
 *   row  7          bank bar (its CURRENT-page segment is 2px, so that one
 *                   segment also occupies row 8; the rest of the bar does not)
 *   row  8          clear except under that segment
 *   row  9          top arms          <- 1px off the bar
 *   rows 10..52     glyph rows (10..16, 19..25, 28..34, 37..43, 46..52)
 *   row  53         bottom arms       <- 1px off the rule
 *   row  54         clear
 *   row  55         footer rule
 *
 * The side arms run 9..15 and 47..53, which pass through the gaps between
 * glyph rows and never touch a glyph. Nothing is given up: still five rows.
 */
const MENU_FRAME_X = 4, MENU_FRAME_Y = 9, MENU_FRAME_W = 120;
/* One clear row above the frame and one below, hence the 1 on each side. */
const MENU_FRAME_BOTTOM_INSET = 1;
/* 4, matching the divable-cell bracket in render_page_movy. It was 7, whose
 * top-left arm ran x=4..10 on row 9 -- directly over where the first list row
 * begins now that the caret is gone, so the two merged into one blob in the
 * corner. Shorter arms clear the text AND read the same: a bracket is a corner
 * tick, not a border. */
const MENU_BRACKET_LEN = 4;

/*
 * The scroll-arrow column for a page-chrome list.
 *
 * drawMenuList's default is 120, which is right for a list that owns the whole
 * 128px screen: the arrow occupies 120..124 and nothing else is out there. This
 * list does not own the screen — MENU_FRAME's right arm is the column x=123,
 * rows 47..53, and an INERT menu longer than five entries draws both (the
 * brackets and a down arrow at 120..124, rows 52..54). They would land on top of
 * each other in the bottom-right corner and read as a smudge, not as two marks.
 *
 * The frame's exclusion zone on the right is x >= 117 on rows 9 and 53 (the
 * corner arms) plus the whole column x=123. 114 is NOT far enough — rendered
 * and looked at: the down arrow's foot lands on row 53 at x=115 and x=117, one
 * pixel off the arm and then on it, and the two read as a single blob in the
 * corner. 110 clears the arm by three columns and the arrow reads as an arrow.
 *
 * drawMenuList derives the right-hand value edge from this, so values pull in to
 * 108 while an arrow is on screen and go back out to 118 when it is not.
 */
const MENU_LIST_INDICATOR_X = 122;

/*
 * THE ONE LIST, as a page-chrome list.
 *
 * Every row on a menu page, an items page and the section picker is drawn here,
 * by the same drawMenuList that draws the slot editor, Master FX, the file
 * browser and the enum picker. Before this it was renderPicker, which was
 * *similar* — same rect, same five rows, same 9px stride — and similar is what
 * this project exists to end.
 *
 * WHAT MOVED, so nobody has to rediscover it from a photograph:
 *
 * 1. THE SCROLL WINDOW. renderPicker centred the cursor:
 *        first = clamp(cur - floor(rows/2), 0, total - rows)
 *    drawMenuList reserves the last row instead (`keepOffLastRow`):
 *        first = max(0, sel - (rows - 2))
 *    For a 10-entry list in a 5-row window:
 *        sel=0  picker 0..4  (sel on row 0)   list 0..4  (sel row 0)   same
 *        sel=3  picker 1..5  (sel on row 2)   list 0..4  (sel row 3)
 *        sel=5  picker 3..7  (sel on row 2)   list 2..6  (sel row 3)
 *        sel=9  picker 5..9  (sel on row 4)   list 6..9  (sel row 3)
 *    So: the cursor now settles on the FOURTH row rather than the middle one,
 *    and at the very end of a list the window shows four entries with the fifth
 *    row blank rather than five with the cursor on the bottom row. That is
 *    accepted, not incidental — it is how every other list in the shadow UI
 *    already scrolls, and one list means one scroll rule.
 *
 * 2. Rows gain a "> " cursor prefix, the selected row's fill spans the whole
 *    128px width rather than stopping at the rect, and scroll arrows appear
 *    (renderPicker drew none at all, so a long list gave no sign it continued).
 *
 * 3. Labels truncate a little sooner: drawMenuList budgets a fixed 6px per glyph
 *    and pays 2 glyphs for the cursor prefix, where renderPicker measured the
 *    proportional font with fitText. ~14 chars against ~21 on a valueless row.
 *
 * `announce: false` is not optional. This controller already announces every
 * menu / items / section move itself (announceEntry, announcePageChange, the
 * picker's own announce) with position and value; letting drawMenuList announce
 * as well would say everything twice to a screen-reader user.
 *
 * EXPORTED, and chain_editor_chrome.mjs's module picker draws through it too.
 * That file already imports MENU_LIST_X/Y/W from here, with the note "imported
 * rather than restated: the two pickers must occupy the SAME rectangle, and a
 * second copy of 'x 8, y 10, w 112' is how they would come to stop doing so".
 * The row renderer is the same argument one level up: converting the page
 * chrome and leaving the module picker on renderPicker would put two different
 * lists in one rectangle, which is the drift this whole exercise exists to end.
 * The picker draws no bracket frame, so the arrow column is more conservative
 * there than it needs to be — identical is worth more than six pixels.
 */
/*
 * `editMode` is drawMenuList's OWN affordance — the selected row's value prints
 * as `[value]` — not a new one. §3.2 of the design named it the survivor of the
 * four spellings (`< value >`, `[value]`, a bracketed LABEL, and nothing at all)
 * that the six hand-rolled row loops had drifted into, so the knob page drawn as
 * a list inherits it rather than inventing a fifth.
 */
export function drawPageChromeList(ctx, rect, entries, index, { editMode = false } = {}) {
    drawMenuList({
        ctx,
        items: entries || [],
        editMode: !!editMode,
        selectedIndex: (index | 0) < 0 ? -1 : (index | 0),
        listArea: { topY: rect.y, bottomY: rect.y + rect.h },
        labelX: rect.x,
        indicatorX: MENU_LIST_INDICATOR_X,
        indicatorBottomY: rect.y + rect.h,
        getLabel: (e) => (e && e.name != null ? String(e.name) : ""),
        /* A menu entry carries a value; a section carries a page count, and a
         * one-page section says nothing rather than "1". */
        getValue: (e) => {
            if (!e) return "";
            if (e.value !== undefined && e.value !== null && e.value !== "") return String(e.value);
            return e.pages > 1 ? String(e.pages) : "";
        },
        valueAlignRight: true,
        /* The label floor below makes long values truncate, which on a knobs
         * page is most of them: "1/2 bar" becomes "1/2...", "kick_01.wav"
         * becomes "kick...". That is the right trade for the UNSELECTED rows —
         * a readable label beats a readable value when you are choosing which
         * row to act on — but it would leave the row you are actually ON
         * unreadable too, and there is no other way to see the full value from
         * here. So the selected row's value marquees, exactly as its label
         * already does. Costs nothing when the value fits. */
        scrollSelectedValue: true,
        /* `valueX` is the LEFT FLOOR a right-aligned value may not cross, and
         * its default (92) is calibrated for a value edge at 126. Here the edge
         * is 118, or 108 with an arrow on screen, and the floor then wins: "Off"
         * right-aligned to 108 wants x=90, gets pushed to 92, and is truncated
         * to fit the 16px left over — it rendered as "Of". renderPicker had no
         * floor at all (it right-aligned and fitted the LABEL to what was left),
         * so the floor goes to the list's own left edge and the label budget,
         * which drawMenuList already derives from the value's position, does the
         * work. Found by rendering it and looking, not by reading it.
         *
         * Handing the floor over like this is also why this is the ONE list that
         * needed drawMenuList's `minLabelChars` — with no value-column floor of
         * its own, a wide value took the whole row and left `A...` / `B...` for
         * two different samples. The general floor is drawMenuList's default, so
         * nothing is passed here; see THE LABEL FLOOR in menu_layout.mjs and
         * tests/host/test_list_label_floor.sh. */
        valueX: rect.x,
        /* Values end at x=118, not the 126 a full-width list uses: the frame's
         * right arm is the column x=123 and it runs through the first and last
         * glyph rows. This is the number renderPicker used (r.x + r.w - 2) and
         * the number the MENU_FRAME comment above is written against. */
        valuePaddingRight: 10,
        announce: false,
    });
}
import { knobInit, knobStep } from "../knob_engine.mjs";
import { formatParamForSet, learnEnumWireFormat, enumWireValue } from "../param_format.mjs";
import { announcePage, announceTouch, announceTurn, announcePageContents } from "./announce_page.mjs";

/** Ticks a key ignores incoming reads after being turned (~200 ms at 44 Hz). */
export const SETTLE_TICKS = 9;

/**
 * Minimum gap between announcements for the SAME key while it is being
 * turned continuously. A fast physical spin decodes to one MIDI CC message
 * per detent — measured on device at up to ~286/s during a fast Braids turn
 * — and every one of those was reaching `announce()`, which always writes to
 * shared memory and bumps a sequence number for the screen-reader consumer
 * to pick up (`host_send_screenreader`, `src/shadow/shadow_ui.c`) whether or
 * not TTS is actually speaking. No one can follow 286 announcements a
 * second, sighted or not, and competing with that many per-detent writes for
 * the same tick budget as rendering was the real cause behind the frame rate
 * dropping under a fast turn (17fps idle -> 5fps while flooded) — this
 * throttle is a genuine UX fix on its own merits, and it happens to be the
 * perf fix too. */
export const ANNOUNCE_THROTTLE_MS = 120;

/**
 * Minimum gap between setParam WRITES for the same key while it is being
 * turned continuously.
 *
 * Measured on device: a fast physical spin decodes to 250-320 MIDI CC
 * messages/second (one per detent), and — confirmed by bypassing it —
 * `setParam` per detent is what was dropping the grid's own redraw rate from
 * ~17fps to 5fps under that load, not rendering (every draw primitive
 * measures near-zero) and not `announce()` (throttling that alone changed
 * nothing). 50 writes/sec is already finer than a human ear or a knob's own
 * declared `step` resolution needs during a fast sweep — the value the
 * screen shows and the value used for the next detent's math (`s.values`,
 * `knobStates`) update on EVERY detent regardless; only the outbound
 * `setParam` IPC call is paced. A write that misses this window is not
 * dropped — see `pendingWrite` below — it is caught by the next tick or by
 * release, so the final settled value always reaches the device exactly. */
export const SETPARAM_THROTTLE_MS = 20;

/**
 * How many modulated params get a live re-read per tick.
 *
 * The staggered cursor exists because eight values is eight IPC round trips,
 * and it works because a human turns one knob at a time. A modulation source
 * breaks that: those values move on their own, continuously, and on the shared
 * cursor each one refreshes only every `stops` ticks — about 5Hz, which
 * against a 1/8-note LFO is undersampled enough that the dot wanders instead
 * of sweeping.
 *
 * Modulated keys therefore get their own lane, ONE per tick, rotating.
 *
 * One, not three. Three was the first guess and it cost more than it bought:
 * at ~2.8ms a read that is 8.4ms of blocking on top of the cursor read, every
 * tick, and measured on device it dragged the whole UI from 42 ticks/sec down
 * to ~28 — visible as dropped frames everywhere, to make a dot smoother that
 * was already smooth. The common case is a single modulated param on the
 * page, and that gets the full tick rate either way; three modulated params
 * now refresh at ~14Hz each instead of 42Hz, which still reads as motion.
 *
 * Cheaper per tick than the old per-draw `:modulated` polling it replaced, so
 * the fast lane is not a net cost.
 *
 * The real fix for the many-modulated case is publishing effective values in
 * shared memory the way slot mute/solo now is — the shim already computes
 * them every block, and it would cost nothing per frame instead of 2.8ms.
 */
export const MOD_FAST_READS_PER_TICK = 1;

/**
 * How long the header keeps following a knob that was TURNED but is not held.
 *
 * A claim made by touch is given up by the note-off. A claim made by a turn
 * alone has no such event — nothing is under a finger — so it has to time out
 * or the cell it claimed stays inverted for the rest of the session. That was
 * the "Shape cell stays highlighted after its value changes" report: an enum
 * you nudge, on a knob whose capacitive pad did not register the nudge.
 *
 * Long enough to read the name and value you just changed, short enough that
 * it reads as a readout rather than as a stuck cell.
 */
export const TURN_CLAIM_MS = 1200;

/** How many times a page will re-read the contract waiting for late metadata. */
export const META_RETRY_LIMIT = 8;
/** Ticks between those attempts (~1 s at the shadow UI's 344 Hz tick).
 *  Paced by wall-clock rather than by page sweeps: an 8-key page wraps every
 *  9 ticks, which would burn the whole retry budget in under two seconds —
 *  long before a module that loads a ROM has finished. */
export const META_RETRY_INTERVAL_TICKS = 344;

/**
 * Ticks between a SELECTION and the contract re-read it earns (~500 ms).
 *
 * A selection can republish the whole parameter set — that is what choosing a
 * preset, a soundfont or a CLAP effect IS — but it does not republish it
 * SYNCHRONOUSLY. schwung-airwindows updates its selected index inside
 * set_param, so `plugin_name` is instantly right, and then schedules the
 * actual plugin load on a worker thread 300 ms later
 * (clap_fx.cpp:806-822, PLUGIN_LOAD_DEBOUNCE_MS). `chain_params` is generated
 * from the LOADED plugin, so for that whole window it still describes the
 * previous effect. Reading the instant the write lands therefore caches the
 * wrong contract: reported from hardware as the grid sitting exactly one
 * selection behind.
 *
 * So the read waits, and the deadline is RE-ARMED on every detent: a spin down
 * a 519-effect list costs one read at the end, not 519 on the way.
 *
 * In MILLISECONDS, not ticks. The thing being waited out is another process
 * debounce, which is in milliseconds, and the shadow UI tick rate is neither
 * fixed nor owned by this file.
 *
 * A deadline is a guess about someone else's debounce, which is why it is
 * backed by the bounded re-arm below rather than trusted on its own. Where a
 * module implements `is_loading` the host uses its ready edge instead
 * (shadow_ui_param_pages.mjs) — that is a genuine signal and a better one.
 * This exists because most of the fleet does not implement it, and correctness
 * cannot be a thing third-party modules have to remember to opt into.
 */
export const CONTRACT_SETTLE_MS = 500;
/**
 * How many times that deadline re-arms while the answer still looks unsettled.
 *
 * Bounded, and small: three reads is ~8 ms of IPC spread over a second and a
 * half. Unbounded would mean a module whose contract legitimately never
 * changes polls forever.
 */
export const CONTRACT_SETTLE_RETRIES = 3;

/**
 * How many times a page will re-try a contract read that FAILED.
 *
 * Bounded for the same reason META_RETRY_LIMIT is: a component whose channel
 * never answers must cost a fixed number of reads, not one per interval for
 * the rest of the session.
 */
export const CONTRACT_RETRY_LIMIT = 40;
/**
 * Ticks between those attempts. Much tighter than META_RETRY_INTERVAL_TICKS —
 * a failed claim is transient (the param channel unblocks as soon as whatever
 * was hogging it returns), and until it clears the grid is showing the
 * PREVIOUS page set, or none at all on a first load. One read per interval
 * while unresolved and zero once resolved, so the read budget is unaffected in
 * the normal case.
 */
export const CONTRACT_RETRY_INTERVAL_TICKS = 30;
/**
 * After the retry budget is spent, how often to quietly probe again.
 *
 * Giving up releases the screen — holding it forever on a component that will
 * never answer is worse than drawing the no-page fallback — but before this
 * existed, giving up ALSO meant never asking again, so the component stayed
 * blank for the rest of the session and only a navigate-away-and-back fixed
 * it. Reported from the device: loading tablor drew a blank chain, and
 * "switching to another chain and back I saw it".
 *
 * The cause there was a module copying 116 files (20.4 MB) inside
 * create_instance, on the SPI callback — so the contract genuinely could not
 * be read for several seconds, and no retry budget is the right answer to
 * that. What IS ours is that the recovery has to be automatic.
 *
 * Ten seconds: slow enough to be free (one read per interval, and only while
 * the component is unreadable), fast enough that a user who steps away and
 * looks back finds it working.
 */
export const CONTRACT_RECOVER_INTERVAL_TICKS = 600;

/* How long a trigger press stays interesting to the renderer (its burst is
 * still on screen), and how many overlapping presses to keep. Both exist only
 * to bound the list — the renderer decides what it actually draws. */
const TRIGGER_BURST_KEEP_MS = 400;
const TRIGGER_BURST_MAX = 4;

export function createController(io = {}) {
    const getParam = io.getParam || (() => null);
    const setParam = io.setParam || (() => {});
    const announce = io.announce || (() => {});
    /* Optional: is this param currently driven by a modulation source? The
     * library cannot answer that — it is host state — so it is injected, and
     * defaults to "no" for callers that have no modulation. */
    const isModulated = io.isModulated || (() => false);
    /*
     * Optional: how the HOST wants a value read on a given surface.
     *
     *   formatValue(fullKey, raw, surface) -> string | null
     *   surface: "cell" (a 30px label band) | "header" (the held-knob strip,
     *            also what the screen reader speaks)
     *
     * For values whose reading cannot be declared statically, the way an enum's
     * can with options/short_options. An LFO's target is stored as "fx1" and
     * reads "FX 1: Room Size" — only the host knows what is loaded in fx1, and
     * resolving it costs IPC, so both the lookup and its caching stay on the
     * host side of the injection. Returning null falls back to the ordinary
     * displayValue path, so a formatter can answer for one key and ignore the
     * rest.
     *
     * Given the FULL key, like getParam and setParam — the renderer works in
     * bare page keys and an io that had to handle both spellings would be an
     * invitation to handle one of them wrong.
     */
    const formatValue = io.formatValue || null;
    const now = io.now || (() => Date.now());
    /* Graphics default on; a caller can pass `enableViz: false` to keep the
     * plain grid (a tool that wants every cell individually addressable), and
     * `vizOverrides` to correct a wrong detector guess without a module
     * release — see viz.mjs resolveViz. */
    const vizEnabled = io.enableViz !== false;
    const vizOverrides = io.vizOverrides || null;
    /*
     * The trailing pages, re-evaluated on every plan.
     *
     * A function rather than an array because the rows are conditional — Save
     * and Delete mean nothing with no preset loaded — and the page set outlives
     * those conditions. Same shape as SLOT_GRID_ACTIONS' always-or-hasPreset
     * filter, just evaluated by the host instead of filtered here: the
     * controller does not know what a preset is.
     */
    const trailingMenus = () =>
        (typeof io.trailingMenus === "function" ? (io.trailingMenus() || []) : []);

    const s = {
        slot: 0,
        component: "synth",
        prefix: "synth",
        pages: [],
        pageIndex: 0,
        fingerprint: null,
        metaIndex: null,
        layout: LAYOUT_DIAL,
        revealValues: false,
        touched: -1,
        values: Object.create(null),
        decorations: null,
        /* staggered read cursor */
        cursor: 0,
        /* key -> last-read modulation flag, refreshed on the read cursor
         * rather than per cell per draw. See tick(). */
        modCache: Object.create(null),
        /* Selected child per child-level, by level key. See childResolve(). */
        childIndex: Object.create(null),
        /* A page name to land on once the pages exist; see restorePage(). */
        restoreName: null,
        /* key -> live modulated ("effective") value, for the dot on the arc.
         * Only modulated keys are in here, and they get their own fast lane in
         * tick() because they are the only values that move on their own. */
        modValues: Object.create(null),
        /* Rotates over the modulated keys, so the fast lane stays bounded. */
        modCursor: 0,
        /* key -> tick at which reads may resume */
        settleUntil: Object.create(null),
        tickCount: 0,
        knobStates: Object.create(null),
        /* key -> ms of the last announce() for that key — see ANNOUNCE_THROTTLE_MS */
        lastAnnounceMs: Object.create(null),
        /* key -> ms of the last setParam() WRITE for that key, and key -> the
         * latest computed wire value still waiting to be written because it
         * arrived inside the throttle window — see SETPARAM_THROTTLE_MS. */
        lastWriteMs: Object.create(null),
        pendingWrite: Object.create(null),
        /* the caller acts on these; the controller never opens a screen itself */
        pending: null,
        /* Page picker: the answer to 76 pages. Open, jog to scroll, click to
         * jump. Held here rather than in the host because it is navigation over
         * the page set, which is what this module is for. */
        pickerOpen: false,
        pickerIndex: 0,
        pickerEntries: [],
        /* First-run gesture hint. Shown until the user does literally anything,
         * then gone for the session — a timer would either be too short to read
         * or long enough to be in the way. */
        hintLines: null,
        hintShown: false,
        /* Out-of-band status the UI wants but no module declares in
         * chain_params. Folded into the read cursor rather than polled
         * separately, so it costs one slot in the rotation, not a frame. */
        presetName: null,
        /* Metadata that arrives after the module reports ready. osirus loads a
         * ROM asynchronously and publishes `rom_index` as ["(loading)"]; baked
         * once at load time, that enum would read "(loading)" for the rest of
         * the session. Re-resolution is bounded and latching — see maybeResettle. */
        metaRetries: 0,
        metaSettled: false,
        /* The `<prefix>:ui_hierarchy` read FAILED — the channel would not
         * answer — so we do not know what this component declares and nothing
         * has been planned from it. See load() and maybeReresolveContract. */
        contractUnresolved: false,
        /* Retries exhausted: the screen is released but a slow probe keeps
         * looking, so an unreadable component recovers without the user
         * having to navigate away and back. */
        contractGaveUp: false,
        /* key -> ms when a trigger last fired, for the bang flash. */
        triggerFiredAt: Object.create(null),
        contractRetries: 0,
        /* Wall-clock ms at which a selection-driven contract re-read comes
         * due, or 0 for none pending. Re-armed per detent — see
         * CONTRACT_SETTLE_MS. */
        contractDirtyAt: 0,
        contractSettleTries: 0,
        /* Fingerprint of the previous POST-DEADLINE reading. Two that agree is
         * what "settled" means here; see maybeSettleContract. */
        contractSettleLastFp: null,
        /* null = never asked, true/false = latched. See isLoadingSays. */
        isLoadingSupported: null,
        /* Param keys a visible_if condition reads. A condition is driven by a
         * VALUE, which moves without the declared contract moving, so the
         * fingerprint cannot see it — these are watched explicitly instead.
         * Cheaper and more exact than polling: only these keys can change what
         * is visible, and we already read every key on the page. */
        conditionKeys: new Set(),
        /* Per-section memory of the sub-page you were last on. Elektron's page
         * buttons work this way — pressing [FLTR] returns you to the FLTR page
         * you were using, not to FLTR 1 — and it matters most on the modules
         * where it is most tedious to get back (minijv's tone subtrees are 15
         * pages each). Applies to SECTION jumps only; a fine jog still steps
         * linearly, or you could never walk the set in order. */
        sectionMemory: Object.create(null),
        /* Cursor per MENU page, keyed by page name for the same reason
         * sectionMemory is: a rebuild moves every index. */
        menuCursor: Object.create(null),
        /* Cursor per KNOBS page under LAYOUT_LIST, keyed by page name for that
         * same reason. Indexes knobRows(), not p.keys directly — a sparse page
         * has holes and a list has none, so the row carries the SLOT it came
         * from and every edit is dispatched by slot. */
        knobCursor: Object.create(null),
        /* The entered knob list has handed the jog to the VALUE under the
         * cursor rather than to the row cursor. One flag, not per page: it is
         * cleared on every enter, and nothing reads it unless the page it
         * belongs to is entered. */
        knobEditing: false,
        /* Every knob currently held, oldest first. See onKnobTouch. */
        touchOrder: [],
        /* ms at which a TURN claimed the header with nothing held, or 0.
         * Only such a claim expires — see TURN_CLAIM_MS. */
        turnClaimMs: 0,
        /* Name of the menu page currently ENTERED, or null. */
        menuEntered: null,
        /*
         * The preset browser's live state, per page name.
         *
         * A preset level publishes a COUNT, a current INDEX and the name of
         * whichever preset is selected — there is no way to ask for a list of
         * names, so the page shows the one you are on rather than a window of
         * five. Keyed by page name because a rebuild moves every index, the
         * same reason sectionMemory and menuCursor are.
         */
        preset: Object.create(null),
        /*
         * The runtime item lists — soundfonts, NAM models, JV expansions.
         *
         * Unlike a preset level this one publishes a real LIST, so the page can
         * show five at a time, and scrolling costs nothing: only the click
         * writes. Keyed by page name, like every other per-page memory here.
         */
        items: Object.create(null),
    };

    /*
     * key -> the concrete key on the wire.
     *
     * Identity for an ordinary page. For a page belonging to a CHILD level it
     * resolves the template: minijv's `part_selector` lists `partlevel`, and
     * the DSP serves `sram_part_<n>_partlevel` where n is the child the
     * selector page committed.
     *
     * Only keys ON the current page are resolved -- fullKey is also used for
     * out-of-band reads (ui_hierarchy, chain_params, preset_name), which are
     * not the level's parameters and must pass through untouched.
     *
     * METADATA stays keyed by the BARE name: chain_params declares `partlevel`,
     * not the resolved form, so only the wire key moves.
     */
    const childIndexFor = (level) => {
        const at = s.childIndex[level];
        return (typeof at === "number" && at >= 0) ? at : 0;
    };
    const childResolve = (key) => {
        const p = page();
        if (!p || !p.childLevel || !Array.isArray(p.keys)) return key;
        if (p.keys.indexOf(key) < 0) return key;
        return resolveChildKey(p.childLevel, childIndexFor(p.level), key) || key;
    };
    const fullKey = (key) => `${s.prefix}:${childResolve(key)}`;
    const page = () => s.pages[s.pageIndex] || null;

    /*
     * What the header calls this page.
     *
     * The planned name is the page IDENTITY -- section memory, restorePage and
     * the items state are all keyed by it -- so it must not move. But for a
     * page belonging to a child level it is the wrong thing to SHOW: minijv
     * plans "Edit Parts - 2", where the 2 is the second page OF THE LEVEL, and
     * a user who has just chosen Part 2 reads that as the part. The two
     * numbers collide by coincidence, which is worse than either alone.
     *
     * It did not fit either: 57px against the ~50px the right side gets once
     * the title claims its floor, so it truncated to "EDIT PARTS -" and lost
     * even the wrong number. Reported from the device as exactly that.
     *
     * Numbered among the LEVEL'S OWN knob pages rather than by the planned
     * suffix: the selector took the level's base name, so the first parameter
     * page is planned "- 2" and there is never a "- 1", which would display as
     * "Part 2 - 2" for the FIRST page of Part 2.
     */
    function pageLabel(p) {
        const pg = p || page();
        if (!pg) return null;
        if (!pg.childLevel || pg.kind !== PAGE_KNOBS) return pg.name;
        const label = pg.childLevel.child_label || "Item";
        const at = childIndexFor(pg.level) + 1;
        const siblings = s.pages.filter(
            (q) => q.level === pg.level && q.kind === PAGE_KNOBS);
        const ord = siblings.indexOf(pg);
        return ord > 0 ? `${label} ${at} - ${ord + 1}` : `${label} ${at}`;
    }
    const keyAt = (slot) => {
        const p = page();
        return p && p.kind === PAGE_KNOBS ? (p.keys[slot] || null) : null;
    };
    const metaAt = (slot) => {
        const k = keyAt(slot);
        return k && s.metaIndex ? s.metaIndex.getOrGuess(k) : null;
    };

    /**
     * Point the controller at a component and build its page set.
     * Safe to call repeatedly — it rebuilds only when the declared contract
     * actually changed, and keeps the user's place when it does.
     */
    function load({ slot = 0, component = "synth", prefix, mode, visible } = {}) {
        const nextPrefix = prefix || component;
        /* Whether we are re-reading the SAME component decides what an
         * unresolved read may keep — see below. */
        const sameComponent = (s.slot === slot && s.component === component && s.prefix === nextPrefix);
        s.lastLoadOpts = { mode, visible };
        /* A different component may well implement is_loading even if the last
         * one did not, so the latch is per-component, not per-session. */
        if (!sameComponent) s.isLoadingSupported = null;
        /* Likewise "we gave up on this one" — a new component gets a clean
         * slate, and the retry budget below is already per-component. */
        if (!sameComponent) s.contractGaveUp = false;
        s.slot = slot;
        s.component = component;
        s.prefix = nextPrefix;

        /*
         * A contract read has three answers and only two of them are the same
         * thing (see planPages): JSON is a declaration, "" is "I declare none",
         * and null is "the channel would not answer". The last one is not news
         * about the module, so nothing here may be derived from it.
         *
         * Reads fail legitimately — granny loads a WAV synchronously on the
         * thread that serves param requests, so the read the UI issues on the
         * way back from its file browser times out at 100 ms. What was wrong was
         * treating that silence as "this module has no hierarchy" and
         * paginating chain_params, which put granny's `sample_path` on knob 1.
         */
        const rawHierarchy = getParam(`${s.prefix}:ui_hierarchy`);
        if (rawHierarchy === null || rawHierarchy === undefined) {
            /* A fresh failure gets a fresh retry budget; a repeat one does not,
             * or a channel that never answers would retry forever. */
            if (!s.contractUnresolved) s.contractRetries = 0;
            s.contractUnresolved = true;
            /* Do not latch: the retry loop owns this until the read lands. */
            s.metaSettled = false;
            /* Deliberately NOT read: chain_params on its own cannot produce a
             * plan we are willing to show, so asking for it would just be a
             * second doomed round trip on a channel already refusing. */
            if (sameComponent) {
                /*
                 * Keep the page set we already had.
                 *
                 * The component has not changed, so the plan on screen is still
                 * the plan this component declared — it is stale by at most the
                 * retry interval, and stale-but-right beats a page built from a
                 * failure. The granny case is exactly this: the plan we keep is
                 * byte-identical to the one that arrives.
                 */
                return false;
            }
            /*
             * A DIFFERENT component, though, we know nothing about — keeping the
             * previous one would show one module the other module knobs. Show
             * nothing until the read lands.
             */
            flushDueWritesUnconditionally();
            s.pages = [];
            s.fingerprint = null;
            s.hierarchy = null;
            s.chainParams = null;
            s.metaIndex = null;
            s.conditionKeys = new Set();
            s.values = Object.create(null);
            s.cursor = 0;
            s.pageIndex = 0;
            s.metaRetries = 0;
            s.isLoadingSupported = null;
            s.knobStates = Object.create(null);
            s.lastWriteMs = Object.create(null);
            s.pendingWrite = Object.create(null);
            return true;
        }
        s.contractUnresolved = false;
        s.contractRetries = 0;
        s.contractGaveUp = false;

        const hierarchy = parse(rawHierarchy);
        /*
         * A chain_params read can fail the same way a ui_hierarchy read can,
         * and it is the same defect one key over.
         *
         * null is "the channel would not answer", "" is "nobody serves this
         * key". Collapsing them parsed a TIMEOUT as "this module declares no
         * chain_params", rebuilt the metaIndex from the hierarchy alone, and
         * every knob lost its name — permanently, because for a contract with
         * no enum placeholder nothing ever reads again.
         *
         * The hierarchy answered, so the plan is sound; only the metadata is
         * missing. Keep the metadata we already had, and let the settle below
         * fetch it again.
         */
        const rawChain = getParam(`${s.prefix}:chain_params`);
        const chainFailed = (rawChain === null || rawChain === undefined);
        const chainParams = chainFailed
            ? (sameComponent ? s.chainParams : null)
            : parse(rawChain);
        if (chainFailed && sameComponent) armContractSettle();
        const planned = planPages({ hierarchy, chainParams, mode, visible, trailingMenus: trailingMenus() });
        /* Retained so a visibility re-plan costs no extra device reads. */
        s.hierarchy = hierarchy;
        s.chainParams = chainParams;

        if (planned.fingerprint === s.fingerprint) return false;

        const oldPages = s.pages;
        const oldIndex = s.pageIndex;
        s.pages = planned.pages;
        s.fingerprint = planned.fingerprint;
        s.metaIndex = buildMetaIndex({ hierarchy, chainParams });
        s.conditionKeys = planned.conditionKeys || new Set();
        /* A rebuild mid-turn must not silently drop a throttled write that
         * hasn't reached the device yet. */
        flushDueWritesUnconditionally();
        s.values = Object.create(null);
        s.cursor = 0;
        s.metaRetries = 0;
        s.metaSettled = false;
        s.knobStates = Object.create(null);
        s.lastWriteMs = Object.create(null);
        s.pendingWrite = Object.create(null);
        /* A rebuild after a module finishes loading shifts every index, so land
         * by name rather than by position; a first load lands on a grid. */
        s.pageIndex = oldPages.length ? reanchor(oldPages, oldIndex, s.pages) : firstGrid(s.pages);
        /* A restore that could not be honoured yet gets its chance here.
         * See restorePage(): the pages may not have existed when the caller
         * asked. */
        applyPendingRestore();
        announcePageChange();
        return true;
    }

    /**
     * Land on the page with this NAME, now or as soon as it exists.
     *
     * The caller (returnToParamPagesFromEditor) knows which page you left; it
     * cannot know whether the pages are back yet. Coming out of granny's file
     * browser they are NOT: granny loads the WAV synchronously inside
     * set_param, on the SPI thread that also serves param reads, so the
     * contract read right after a sample selection times out and planPages
     * refuses to invent pages from a failed read. The old restore looked once,
     * found an empty list, and silently gave up -- which is why choosing a
     * sample dropped you on page 1 instead of the page you were on.
     *
     * So the request is REMEMBERED and re-applied whenever the page set is
     * (re)planned. It is one-shot: once honoured, or once the user has moved
     * somewhere themselves, it is dropped rather than fighting them.
     */
    /*
     * `enter` says whether the door OPENS on arrival, and the caller decides
     * because only the caller knows why we came back.
     *
     * A door you were SENT to opens; a door you FINISHED with closes. Backing
     * out of a browser without choosing anything never really left the menu,
     * so it returns you inside it. Completing the thing you came for — loading
     * a preset, saving one — is done, so it hands the jog back to paging.
     * Deleting is management: you are likely to do more of it, so it stays in.
     *
     * Default false, which is what this did before the option existed.
     */
    function restorePage(name, { enter = false } = {}) {
        s.restoreName = (typeof name === "string" && name) ? name : null;
        s.restoreEnter = !!enter;
        applyPendingRestore();
    }

    function applyPendingRestore() {
        if (!s.restoreName) return;
        const pages = s.pages || [];
        for (let i = 0; i < pages.length; i++) {
            if (pages[i] && pages[i].name === s.restoreName) {
                s.pageIndex = i;
                s.restoreName = null;
                /*
                 * Open the door only when the CALLER said to — see restorePage.
                 *
                 * This used to open on every restore, on the reasoning that an
                 * arrival you asked for should not need a second gesture. That
                 * is right for backing out of a browser without choosing
                 * anything (reported from hardware: Load... with nothing saved,
                 * then Back, left the jog outside the menu you had just come
                 * from) and for a delete, which is management you are likely to
                 * continue. It is wrong for finishing: after loading or saving
                 * a preset you are done with the menu and want the jog back.
                 *
                 * Entering costs nothing and writes nothing either way — a
                 * preset browser auditions on TURN, not on entry. A knob page
                 * is not a door, so the ordinary come-back-where-I-was restore
                 * is unaffected whatever the caller asks for.
                 */
                if (s.restoreEnter && isDoor(page()) && !menuEntered()) enterMenu();
                else if (!s.restoreEnter && menuEntered()) exitMenu();
                return;
            }
        }
        /* Not there yet. Left armed for the next plan -- unless the contract
         * has settled, in which case the page genuinely does not exist and
         * waiting forever would hijack a later navigation. */
        if (s.metaSettled) s.restoreName = null;
    }

    /** Poll for a contract that changed underneath us (async ROM/sample loads). */
    function reloadIfChanged(opts) {
        return load({ slot: s.slot, component: s.component, prefix: s.prefix, ...opts });
    }

    /**
     * A selection was made: the contract it publishes comes due shortly.
     *
     * Idempotent and re-arming, which is the whole point — every detent of a
     * jog through a long list pushes the deadline out, so the read happens
     * once, when the hand stops.
     */
    function armContractSettle() {
        s.contractDirtyAt = now() + CONTRACT_SETTLE_MS;
        s.contractSettleTries = 0;
        /* Both agreeing readings must post-date THIS deadline. */
        s.contractSettleLastFp = null;
        /* Where a module DOES implement is_loading, its ready edge fires first
         * and this deadline finds nothing left to change. Cheap either way. */
        s.metaSettled = false;
        s.metaRetries = 0;
    }

    /**
     * Does the contract we just read still describe a module mid-load?
     *
     * schwung-airwindows answers "[]" for the entire dlopen window, because
     * its param cache is filled from the plugin pointer and that pointer is
     * NULL until the load completes (clap_fx.cpp:342-343, 914-916). The chain
     * host now treats "[]" as no answer and substitutes the module.json
     * fallback, which for that module is a single `plugin_id`. Both shapes
     * mean the same thing and neither is the contract we are waiting for.
     *
     * Asked as "does anything on the knob page have declared metadata" rather
     * than by naming either shape, so a third module that degrades a third way
     * is covered without being enumerated.
     */
    function contractLooksUnsettled() {
        if (!s.metaIndex) return true;
        const p = s.pages.find((q) => q && q.kind === PAGE_KNOBS && (q.keys || []).length);
        if (!p) return false;
        return p.keys.every((k) => s.metaIndex.getOrGuess(k).guessed);
    }

    /**
     * Does this component serve `is_loading`, asked at most once per component?
     *
     * An unserved key answers "" — the shim replies with an error and a zeroed
     * buffer — and only an unclaimable channel answers null. So one probe
     * settles it forever: "1"/"0" is a module that implements it, anything
     * else is one of the many that do not, and we never ask again.
     *
     * Only ever consulted while a settle is already pending, so a component
     * nobody selects anything on never spends this read at all.
     */
    function isLoadingSays() {
        if (s.isLoadingSupported === false) return null;
        const raw = getParam(`${s.prefix}:is_loading`);
        if (raw === null || raw === undefined) return null;   /* claim failed */
        if (raw !== "1" && raw !== "0") { s.isLoadingSupported = false; return null; }
        s.isLoadingSupported = true;
        return raw;
    }

    /**
     * Re-read a contract a selection made stale, once it has settled.
     *
     * Returns true when it spent a read.
     *
     * Deciding it HAS settled takes two readings that agree, and both of them
     * must be taken AFTER the deadline. Comparing the first reading against
     * the fingerprint from before the write is the tempting shortcut — it
     * settles a static module in a single probe — but it settles WRONGLY
     * whenever the module load lands just after that first probe, which is the
     * original bug rediscovered one layer up.
     *
     * `is_loading` is the fast path and never the mechanism: while it says "1"
     * a probe costs one cheap read and no contract read at all. It is not
     * allowed to shorten the confirmation — see the note at the bottom.
     */
    function maybeSettleContract(reload) {
        if (!s.contractDirtyAt || now() < s.contractDirtyAt) return false;
        /*
         * Never rebuild under a hand that is on a knob. A rebuild drops
         * s.values and resets the read cursor, so doing it mid-turn makes the
         * cell the user is holding blink back to "--". Deferred, not dropped.
         */
        if (s.touchOrder.length) {
            s.contractDirtyAt = now() + CONTRACT_SETTLE_MS;
            return false;
        }
        /* A failed read has its own bounded retry, which owns this state until
         * it lands; two loops re-reading the same key would just race. */
        if (s.contractUnresolved) return false;

        const rearm = () => { s.contractDirtyAt = now() + CONTRACT_SETTLE_MS; };
        s.contractSettleTries++;
        const capped = s.contractSettleTries >= CONTRACT_SETTLE_RETRIES;

        const loading = isLoadingSays();
        if (loading === "1" && !capped) { rearm(); return true; }

        reload();
        const fp = s.fingerprint;

        if (capped) { s.contractDirtyAt = 0; return true; }
        /* Still describing a module mid-load: keep probing, never plan from it. */
        if (contractLooksUnsettled()) { rearm(); return true; }
        /*
         * Two post-deadline readings that agree — including when is_loading
         * just said "0".
         *
         * Letting a "0" stand in for the second reading was tried and is
         * wrong: a module can report ready while its contract is still the
         * previous one (the fleet fake does exactly this), and then the fast
         * path settles on the stale answer — the original bug, restored by the
         * thing meant to avoid it. is_loading may spend FEWER reads; it may
         * never remove a confirmation.
         */
        if (s.contractSettleLastFp === fp) { s.contractDirtyAt = 0; return true; }
        s.contractSettleLastFp = fp;
        rearm();
        return true;
    }

    /**
     * Is any enum on the current page still showing a placeholder?
     *
     * A module that is still loading publishes a stand-in option set — exactly
     * one entry wrapped in parentheses, "(loading)" — or no options at all.
     * Those are the two shapes worth waiting for; anything else is a real,
     * settled declaration.
     */
    function metaUnsettled() {
        const p = page();
        if (!p || p.kind !== PAGE_KNOBS || !s.metaIndex) return false;
        for (const key of p.keys) {
            const meta = s.metaIndex.getOrGuess(key);
            if (meta.kind !== KIND_ENUM) continue;
            const o = meta.options;
            if (!o) return true;
            if (o.length === 1 && /^\(.*\)$/.test(String(o[0]))) return true;
        }
        return false;
    }

    /**
     * Bounded, latching re-resolve of late metadata.
     *
     * Costs one contract read per interval while something is unsettled and
     * NOTHING once it settles or the retry budget runs out — a module whose
     * enum legitimately reads "(none)" must not make us poll forever.
     */
    function maybeResettle(reload) {
        /*
         * Never LATCH on a contract we could not read.
         *
         * metaUnsettled() inspects the page we are holding, and while the read
         * is failing that page is either the previous component or nothing at
         * all — "looks complete" would be a verdict about the wrong data, and
         * once it sets metaSettled nothing ever reads again. That latch is why
         * the granny symptom survived until a full teardown of the controller
         * rather than clearing itself a frame later. The contract retry below
         * owns this state until the read lands.
         */
        if (s.contractUnresolved) return false;
        if (s.metaSettled || s.metaRetries >= META_RETRY_LIMIT) return false;
        if (!metaUnsettled()) { s.metaSettled = true; return false; }
        s.metaRetries++;
        return reload();
    }

    /**
     * Bounded re-try of a contract read that FAILED.
     *
     * One read per CONTRACT_RETRY_INTERVAL_TICKS while unresolved, capped, and
     * nothing at all once it lands — the grid cannot recover on its own
     * otherwise, because there is no page to hang the ordinary read cursor on.
     */
    function maybeReresolveContract(reload) {
        if (!s.contractUnresolved) return false;
        if (s.contractRetries >= CONTRACT_RETRY_LIMIT) {
            /* Given up. Stop CLAIMING to be mid-resolve: the host holds the
             * screen while this is true, and holding it forever on a component
             * that will never answer is worse than running the ordinary
             * no-drawable-page fallback.
             *
             * Giving up the SCREEN is not giving up the COMPONENT, though —
             * those were the same thing until 2026-08 and that is the bug.
             * A slow probe keeps running so the page comes back on its own. */
            s.contractUnresolved = false;
            s.contractGaveUp = true;
            return false;
        }
        s.contractRetries++;
        return reload();
    }

    /* ------------------------------------------------------------ reading */

    /**
     * One read per tick, cycling the current page. Values arrive over several
     * frames rather than stalling one — the whole point of the cursor.
     */
    /**
     * Catches the case a per-detent flush in onKnobTurn cannot: the hand
     * pauses mid-turn (still touching, so no release event either) with a
     * value sitting in pendingWrite from the last detent before the pause.
     * Nothing else would ever write it out. Cheap when there is nothing
     * pending — the common case — since it is only object-key iteration.
     */
    function flushDueWrites() {
        const t = now();
        for (const key in s.pendingWrite) {
            if (t - (s.lastWriteMs[key] || 0) < SETPARAM_THROTTLE_MS) continue;
            setParam(fullKey(key), s.pendingWrite[key]);
                replanIfCondition(key);
            s.lastWriteMs[key] = t;
            delete s.pendingWrite[key];
        }
    }

    /** Every pending write, ignoring the throttle window — a rebuild (module
     * swap, visible_if re-plan) must never silently drop one. */
    function flushDueWritesUnconditionally() {
        for (const key in s.pendingWrite) {
            setParam(fullKey(key), s.pendingWrite[key]);
                replanIfCondition(key);
        }
    }

    /* Give up a header claim made by a turn once the hand has moved on. Held
     * knobs are exempt: their claim ends with the note-off. */
    function expireTurnClaim() {
        if (!s.turnClaimMs || s.touchOrder.length) return;
        if (now() - s.turnClaimMs < TURN_CLAIM_MS) return;
        s.turnClaimMs = 0;
        s.touched = -1;
    }

    function tick() {
        s.tickCount++;
        flushDueWrites();
        expireTurnClaim();

        /*
         * Re-try an unresolved contract BEFORE the page guards below: an
         * unresolved first load leaves no page at all, so anything hung off the
         * read cursor would never run and the grid would stay blank forever.
         */
        let triedReresolve = false;
        if (s.contractUnresolved && s.tickCount % CONTRACT_RETRY_INTERVAL_TICKS === 0) {
            triedReresolve = true;
            maybeReresolveContract(() => reloadIfChanged(s.lastLoadOpts));
        } else if (s.contractGaveUp && s.tickCount % CONTRACT_RECOVER_INTERVAL_TICKS === 0) {
            /* The budget is spent and the screen is released, but keep
             * looking: whatever was blocking the channel (a module loading a
             * bank on the audio thread) ends eventually, and the user should
             * not have to navigate away and back to find out. Costs one read
             * per interval, and only while the component is unreadable. */
            triedReresolve = true;
            reloadIfChanged(s.lastLoadOpts);
            if (s.contractUnresolved) {
                /*
                 * Still refusing. Put it straight back to given-up.
                 *
                 * A failed reload sets contractUnresolved and, because the
                 * flag had been cleared, hands out a FRESH retry budget — so
                 * without this the slow probe re-arms the fast loop every
                 * time, and "one read every ten seconds" becomes a permanent
                 * 30-tick retry cycle that also re-holds the screen. Caught by
                 * the read-count assertion in test_contract_recovery.sh, which
                 * is why that test counts reads and not just outcomes.
                 */
                s.contractUnresolved = false;
                s.contractGaveUp = true;
            }
        }

        /*
         * The settle runs BEFORE the page-kind guards below, because the
         * selection that made the contract stale is made ON the preset or
         * items page and those return early. Waiting until the user reached a
         * knob page would mean the read never happened while browsing, which
         * is precisely when the labels are wrong.
         */
        if (!triedReresolve) {
            triedReresolve = maybeSettleContract(() => reloadIfChanged(s.lastLoadOpts));
        }

        const p = page();
        if (p && p.kind === PAGE_PRESET) { tickPreset(p); return null; }
        if (p && p.kind === PAGE_ITEMS) { tickItems(p); return null; }
        if (!p || p.kind !== PAGE_KNOBS || p.keys.length === 0) return null;

        refreshModulatedValues(p);

        /* One extra stop in the rotation reads the preset name, which a
         * hardware synth would put in its display and which no module declares
         * as a param. */
        const stops = p.keys.length + 1;
        const at = s.cursor % stops;
        s.cursor = (s.cursor + 1) % stops;

        /* Give late metadata a chance to arrive, on a wall-clock cadence. */
        if (!triedReresolve && s.tickCount % META_RETRY_INTERVAL_TICKS === 0) {
            maybeResettle(() => reloadIfChanged(s.lastLoadOpts));
        }

        if (at === p.keys.length) {
            const pn = getParam(`${s.prefix}:preset_name`);
            s.presetName = (pn && pn.length) ? pn : null;
            return null;
        }
        const key = p.keys[at];
        if (!key) return null;

        /* Do not clobber a value the user is actively turning. */
        if ((s.settleUntil[key] || 0) > s.tickCount) return null;

        /* Refresh this key's modulation flag on the SAME rotation as its value.
         *
         * The renderer asks `modulated(key)` for every cell of every draw, and
         * each of those was a synchronous round trip: measured on device, the
         * `<key>:modulated` reads were 3.5 of the grid's 7.1 reads per tick —
         * half of them — for an indicator that only changes when the user
         * edits a modulation routing. (Worse on a full page: eight knobs is
         * eight round trips per draw, and the no-`:modulated` fallback path
         * costs up to three reads each.)
         *
         * On the cursor it costs one read per tick and the whole page is
         * current within `stops` ticks — under 0.2s, for a tick mark. */
        s.modCache[key] = !!isModulated(fullKey(key));

        /* For a modulated key the plain key returns the EFFECTIVE value — what
         * the source is currently driving it to — and that belongs to the dot.
         * The pointer wants the base, so ask for it directly. Same one read on
         * the cursor either way; the extra cost of showing both values is the
         * fast lane above, not this.
         *
         * `:base` is served by chain_mod_get_base_for_subkey and only exists
         * while a target is active, so fall back rather than blank the knob if
         * the flag and the target ever disagree. */
        let raw = null;
        if (s.modCache[key]) raw = getParam(fullKey(key) + ":base");
        /*
         * "" counts as a MISS, not as a value.
         *
         * A key nobody serves does not answer null — the shim replies with an
         * error and a zeroed buffer, and the JS binding hands back "". So an
         * empty `:base` sailed through this fallback as a legitimate reading,
         * the plain key was never asked, and the cell showed Number("") = 0.
         *
         * On the slot-settings grid that was the visible bug: turn Volume, let
         * go, and ~200ms later — when the settle window expires and the cursor
         * reads again — it dropped to 0%. Volume was never 0; the value the UI
         * held was an empty string. Both halves of the fallback take the same
         * view, so a module that answers "" for a real key is also not
         * mistaken for one that answered zero.
         */
        if (raw === null || raw === undefined || raw === "") raw = getParam(fullKey(key));
        if (raw === null || raw === undefined || raw === "") return null;

        /* First successful read repairs a guessed range, once. */
        const meta = s.metaIndex.getOrGuess(key);
        /*
         * ...and teaches an enum which wire format its plugin speaks. This is
         * THE read detection is allowed to use: it comes from the device, it is
         * already being made, and it keeps arriving, so a verdict is never
         * derived from a value the grid itself wrote. See learnEnumWireFormat.
         */
        learnEnumWireFormat(meta, raw);
        if (meta.guessed) {
            const patch = inferFromValue(meta, raw);
            if (patch) Object.assign(meta, patch);
            delete meta.guessed;
        }
        /* A change to a key that gates visibility re-plans the page set: the
         * params it hides or reveals are not otherwise reachable. */
        const changed = s.values[key] !== raw;
        s.values[key] = raw;
        if (changed) replanIfCondition(key);
        return key;
    }

    /* ------------------------------------------------------------- input */

    /**
     * Open the page picker: one entry per section rather than per page, since a
     * list of 76 pages is the same chore in a different shape. minijv folds to
     * under 25 entries this way.
     */
    /* ------------------------------------------------------------- menu */


    /* ------------------------------------------------------------ items */

    function itemsState(p) {
        const pg = p || page();
        if (!pg || pg.kind !== PAGE_ITEMS) return null;
        let st = s.items[pg.name];
        if (!st) st = s.items[pg.name] = { list: [], cursor: 0, current: -1, read: 0 };
        /*
         * A DERIVED list is built HERE, not in tickItems, because it needs no
         * read and therefore must not depend on tick order. Built there, a
         * click that arrived before the first tick found an empty list and
         * enterMenu refused it -- the selector looked dead until a frame had
         * passed.
         *
         * The level declares how many children and what to call them, so this
         * is arithmetic, not I/O.
         */
        if (Array.isArray(pg.derivedLabels)) {
            const n = pg.derivedLabels.length;
            if (st.list.length !== n) {
                st.list = pg.derivedLabels.map((label, i) => ({ index: i, label }));
                if (st.cursor >= n) st.cursor = Math.max(0, n - 1);
            }
            /* A child selector knows its own answer; a mode selector is told
             * by the module, so its `current` comes from the tick read. */
            if (pg.childOf) st.current = childIndexFor(pg.childOf);
        }
        return st;
    }

    /**
     * Two reads, alternating: the list itself and the current selection.
     *
     * The list is re-read rather than fetched once because it is runtime data —
     * a soundfont appears when the user copies one onto the device — but on the
     * same one-per-tick budget as everything else on this screen.
     */
    function tickItems(p) {
        const st = itemsState(p);
        if (!st) return;
        /*
         * A DERIVED list costs no IPC. The level declares how many children
         * and what to call them, so there is nothing to read and nothing that
         * can go stale -- and `current` comes from local state rather than
         * from a module param.
         */
        /*
         * The LIST is already built and cannot go stale. A child selector has
         * nothing else to ask for either -- the selection is ours. A MODE is
         * the module's state, so its current value is still read, on the same
         * one-per-tick budget.
         */
        if (Array.isArray(p.derivedLabels)) {
            if (!p.selectParam || p.childOf) return;
            const raw = getParam(fullKey(p.selectParam));
            if (raw === null || raw === undefined) return;   /* failed read: not news */
            const at = p.derivedLabels.findIndex(
                (l) => String(l).toLowerCase() === String(raw).trim().toLowerCase());
            /* The module answers "Performance" where the hierarchy says
             * "performance", so the match is case-insensitive. A numeric
             * answer is an index. */
            if (at >= 0) st.current = at;
            else {
                const n = parseInt(raw, 10);
                if (isFinite(n) && n >= 0 && n < st.list.length) st.current = n;
            }
            return;
        }
        const at = st.read % 2;
        st.read++;
        if (at === 0) {
            const parsed = parse(getParam(fullKey(p.itemsParam)));
            if (Array.isArray(parsed)) {
                st.list = parsed.map((it, i) => ({
                    index: (it && it.index !== undefined) ? it.index : i,
                    label: String((it && (it.label || it.name)) || `Item ${i + 1}`),
                }));
                if (st.cursor >= st.list.length) st.cursor = Math.max(0, st.list.length - 1);
            }
        } else if (p.selectParam) {
            const n = parseInt(getParam(fullKey(p.selectParam)), 10);
            if (isFinite(n)) st.current = n;
        }
    }

    /** Move the highlight. No device write — choosing is the click. */
    function stepItems(delta) {
        const st = itemsState();
        if (!st || !st.list.length) return false;
        const before = st.cursor;
        st.cursor = Math.max(0, Math.min(st.list.length - 1, st.cursor + delta));
        if (st.cursor === before) return false;
        const it = st.list[st.cursor];
        announce(`${it.label}, ${st.cursor + 1} of ${st.list.length}`);
        return true;
    }

    /**
     * Commit the highlighted item and leave.
     *
     * Where to leave TO is the declaration's business: a level with navigate_to
     * is saying "having chosen, you want to be here". Without one, the first
     * grid page, same as the preset browser. A named level that plans both a
     * preset browser and a grid means the browser — see below.
     */
    function commitItem() {
        const p = page();
        const st = itemsState(p);
        if (!st || !st.list.length) return false;
        const it = st.list[st.cursor];
        if (p.modeSelect) {
            /*
             * A mode RE-ROOTS the hierarchy: the level named by the mode
             * becomes the walk root, so every page after this one is a
             * different page. The param write below tells the module; this
             * tells the planner, and re-plans from the cached contract so the
             * new pages are there before the next frame.
             *
             * armContractSettle further down books a re-read as well, because
             * the module may republish once it has switched -- minijv resets
             * its emulator to change mode, which is not instant.
             */
            const chosen = (p.derivedLabels || [])[it.index];
            if (chosen !== undefined) {
                if (s.lastLoadOpts) s.lastLoadOpts.mode = chosen;
                else s.lastLoadOpts = { mode: chosen };
            }
        }
        if (p.childOf) {
            /*
             * A child is chosen LOCALLY -- there is no param to write. It
             * re-keys the level's own pages, so the same `partlevel` cell now
             * addresses a different part, and every value cached for that
             * level belonged to the previous one. Dropping them is what stops
             * the old part's numbers sitting under the new part's labels.
             */
            s.childIndex[p.childOf] = it.index;
            for (const pg of s.pages) {
                if (pg.level !== p.childOf || !Array.isArray(pg.keys)) continue;
                for (const k of pg.keys) {
                    delete s.values[k];
                    delete s.pendingWrite[k];
                    delete s.knobStates[k];
                }
            }
            s.cursor = 0;
        } else if (p.selectParam) {
            setParam(fullKey(p.selectParam), String(it.index));
        }
        st.current = it.index;
        /*
         * The chosen item can republish the whole contract — a different
         * soundfont has different presets — so let it be re-read.
         *
         * Clearing metaSettled is NOT enough on its own, and that gap is the
         * airwindows bug: maybeResettle only fires for an enum still showing a
         * placeholder, so a float-only contract re-latched on the next tick
         * having read nothing. armContractSettle is what actually books the
         * read.
         */
        armContractSettle();
        if (p.modeSelect) replanForMode();
        s.menuEntered = null;
        let target = -1;
        if (p.navigateTo) {
            /*
             * A level can produce TWO pages -- a preset browser and a knob
             * grid -- and naming it did not say which. Prefer the browser.
             *
             * This is obxd, reported from the device as "jump to category
             * lands on knobs, not the preset list": its `banks` level declares
             * navigate_to `root`, and root carries list_param/count_param AND
             * knobs. Filtering to PAGE_KNOBS could only ever find the grid, so
             * choosing a bank landed you on the sliders rather than in that
             * bank's presets. A chooser that filters a list means "now show me
             * the list" -- that is the whole reason it exists.
             *
             * Preferring rather than adding a `navigate_to: {level, kind}`
             * form is deliberate. Only three modules in the fleet declare
             * navigate_to at all, and the other two (303, jv880) name levels
             * with no preset page, so they are untouched -- while a new
             * declaration would repeat today's `options_as_string` lesson,
             * which was documented for months and set by nobody.
             */
            const at = (kind) => s.pages.findIndex((q) => q.level === p.navigateTo && q.kind === kind);
            target = at(PAGE_PRESET);
            if (target < 0) target = at(PAGE_KNOBS);
        }
        /*
         * A child selector navigates to ITS OWN level's parameters. Those are
         * the pages the choice just re-keyed, and there is nowhere else the
         * gesture could sensibly mean. Without this it fell through to
         * firstGrid and landed on the module's first page -- you chose Part 3
         * and were shown Main.
         */
        if (target < 0 && p.childOf) {
            target = s.pages.findIndex(
                (q) => q.level === p.childOf && q.kind === PAGE_KNOBS);
        }
        if (target < 0) target = firstGrid(s.pages);
        announce(it.label);
        /*
         * A door you were SENT to opens — see goToPage. Reported from the
         * device: "factory does dump me to presets, but shouldn't presets be
         * already active? I have to click into it." Entering writes nothing; a
         * browser auditions on TURN, so this hands you the jog without loading.
         */
        if (target >= 0) goToPage(target, { remember: false, enterIfDoor: true });
        return true;
    }

    /* ---------------------------------------------------------- presets */

    /** Live state for the preset page on screen, created on first sight. */
    function presetState(p) {
        const pg = p || page();
        if (!pg || pg.kind !== PAGE_PRESET) return null;
        let st = s.preset[pg.name];
        if (!st) st = s.preset[pg.name] = { count: 0, index: 0, name: null, read: 0 };
        return st;
    }

    /** "Fat Bass, 12 of 2427" — what the page says and what it announces. */
    function presetSpoken() {
        const st = presetState();
        if (!st) return "";
        const name = st.name || `Preset ${st.index + 1}`;
        return st.count > 0 ? `${name}, ${st.index + 1} of ${st.count}` : name;
    }

    /**
     * Move the selection, which LOADS that preset — the browser auditions, the
     * way the list editor always has.
     *
     * Writes the index and re-reads the name, so it costs one write and one
     * read per detent. That is affordable because it only happens while the
     * page is entered and the jog is being turned, and it is the whole point
     * of the gesture; it is not on the idle path.
     */
    function stepPreset(delta) {
        const p = page();
        const st = presetState(p);
        if (!st || !p || st.count <= 0) return false;
        let next = st.index + delta;
        if (next < 0) next = st.count - 1;
        if (next >= st.count) next = 0;
        if (next === st.index) return false;
        st.index = next;
        setParam(fullKey(p.listParam), String(next));
        /* Hold off the read cursor for the same reason a turned knob does: a
         * read issued before this write lands after it. */
        s.settleUntil[p.listParam] = s.tickCount + SETTLE_TICKS;
        const nm = getParam(fullKey(p.nameParam));
        st.name = (nm && nm.length) ? nm : null;
        /* The new preset can publish a different parameter set — that is what
         * a preset IS — so let the contract be re-read rather than leaving the
         * knob pages describing the preset you just left. Deadline, not an
         * immediate read: the module may still be loading it, and every detent
         * re-arms so a fast spin costs one read rather than one per step. */
        armContractSettle();
        /* Throttled exactly as a turned knob is: a fast spin down a 2427-preset
         * list is hundreds of announcements a second, which no one can follow
         * and which competes with the redraw for the same tick. */
        const nowMs = now();
        if (nowMs - (s.lastAnnounceMs[p.listParam] || 0) >= ANNOUNCE_THROTTLE_MS) {
            s.lastAnnounceMs[p.listParam] = nowMs;
            announce(presetSpoken());
        }
        return true;
    }

    /**
     * One preset read per tick, cycling count -> index -> name.
     *
     * On the same budget as the knob cursor and for the same reason: three
     * synchronous round trips in one frame is most of the frame. The page
     * shows "--" until they land, which takes ~3 ticks.
     */
    function tickPreset(p) {
        const st = presetState(p);
        if (!st) return;
        const at = st.read % 3;
        st.read++;
        if (at === 0) {
            const c = getParam(fullKey(p.countParam));
            const n = parseInt(c, 10);
            if (isFinite(n) && n >= 0) st.count = n;
        } else if (at === 1) {
            /* Not while the user is turning: a read issued before the write
             * lands after it and drags the selection backwards, exactly as it
             * would for a knob. */
            if ((s.settleUntil[p.listParam] || 0) > s.tickCount) return;
            const v = getParam(fullKey(p.listParam));
            const n = parseInt(v, 10);
            if (isFinite(n) && n >= 0) st.index = n;
        } else {
            const nm = getParam(fullKey(p.nameParam));
            st.name = (nm && nm.length) ? nm : null;
            /*
             * The HEADER's name too, from this same read.
             *
             * s.presetName is otherwise only refreshed by the knob page's
             * rotation, and this branch returns before that -- so scrolling a
             * preset browser changed the sound while the header kept naming
             * the preset you started on, and it only caught up once you
             * navigated to the knobs. Reported from the device for airwindows,
             * whose browser is the module identity: "it only updates after
             * going to the knobs, not on preset change".
             *
             * Free: it is the read that just happened, not a new one. A
             * browser's name_param IS the name of the current selection, which
             * is exactly what the header wants -- airwindows spells it
             * `plugin_name` rather than `preset_name`, so keying off the name
             * the page declares is also what makes it work there.
             */
            if (st.name) s.presetName = st.name;
        }
    }

    /* Cursor per MENU page, by page NAME — page indices move on rebuild. */
    function menuIndex(p) {
        if (!p || p.kind !== PAGE_MENU) return 0;
        const n = (p.entries || []).length;
        const cur = s.menuCursor[p.name] || 0;
        return Math.max(0, Math.min(n - 1, cur));
    }
    function setMenuIndex(p, i) {
        if (!p || p.kind !== PAGE_MENU) return;
        const n = (p.entries || []).length;
        s.menuCursor[p.name] = Math.max(0, Math.min(n - 1, i));
    }
    /** The highlighted entry on a menu page, or null. */
    function menuEntry() {
        const p = page();
        if (!p || p.kind !== PAGE_MENU) return null;
        return (p.entries || [])[menuIndex(p)] || null;
    }

    /* ------------------------------------------------- knobs, as a list */

    /** Is THIS page a knob page being drawn as rows? Layout plus page kind. */
    function knobsAsList(p) {
        const pg = p === undefined ? page() : p;
        return !!(s.layout === LAYOUT_LIST && pg && pg.kind === PAGE_KNOBS
                  && Array.isArray(pg.keys) && pg.keys.some(Boolean));
    }

    /*
     * The page's params as rows, each carrying the KNOB SLOT it came from.
     *
     * A page's `keys` can be sparse — branchage's fourth page leaves knob
     * positions unassigned, and the grid draws those as gaps. A list has no
     * gaps, so row 3 of the list is not knob 3 of the page. Carrying the slot is
     * what lets every edit below be dispatched through the grid's own
     * onKnobTurn(slot, ...) instead of a parallel write path.
     */
    function knobRows(p) {
        const pg = p === undefined ? page() : p;
        if (!pg || pg.kind !== PAGE_KNOBS || !Array.isArray(pg.keys)) return [];
        const out = [];
        for (let i = 0; i < pg.keys.length; i++) {
            if (pg.keys[i]) out.push({ slot: i, key: pg.keys[i] });
        }
        return out;
    }
    function knobRowIndex(p) {
        const pg = p === undefined ? page() : p;
        if (!pg) return 0;
        const n = knobRows(pg).length;
        return Math.max(0, Math.min(n - 1, s.knobCursor[pg.name] || 0));
    }
    function setKnobRowIndex(p, i) {
        const pg = p === undefined ? page() : p;
        if (!pg) return;
        const n = knobRows(pg).length;
        s.knobCursor[pg.name] = Math.max(0, Math.min(n - 1, i));
    }

    /*
     * A row's value, as a string.
     *
     * `displayValue` is the renderer's own — the same function that fills the
     * grid's label band under a held knob and the held-knob header strip — so
     * there is exactly one reading of a value in the engine and the two layouts
     * cannot drift. It is deliberately reached by IMPORT rather than reproduced:
     * a second formatter here is the failure the whole one-list exercise exists
     * to prevent, and it would be invisible until someone declared a `unit`.
     *
     * The host formatter is asked for its "header" form, not its "cell" form,
     * for the same reason renderPageMovy asks the header for one: this is a
     * surface with room. That is the same split `short_options` makes for enums,
     * and it is the mechanism §5.5 relies on rather than a per-surface case.
     *
     * NO DEVICE READ HAPPENS HERE. Values come out of `s.values`, filled by the
     * staggered read cursor, exactly as the grid's do — a param read is ~2.8ms
     * against a 1.68ms whole-page render, so reading on the draw path would cost
     * more than the screen.
     */
    function knobRowValue(key) {
        const meta = s.metaIndex ? s.metaIndex.getOrGuess(key) : null;
        const raw = s.values[key] === undefined ? null : s.values[key];
        if (formatValue) {
            const resolved = formatValue(fullKey(key), raw, "header");
            if (resolved !== null && resolved !== undefined) return String(resolved);
        }
        return displayValue(raw, meta);
    }

    function knobListEntries(p) {
        const pg = p === undefined ? page() : p;
        return knobRows(pg).map(({ key }) => {
            const meta = s.metaIndex ? s.metaIndex.getOrGuess(key) : null;
            /* The FULL label, not labelForCell's five-character mnemonic: that
             * budget is a property of a 32px cell, and this row has ~90px. */
            return { name: (meta && (meta.label || meta.key)) || key,
                     value: knobRowValue(key) };
        });
    }

    /*
     * Landing on a row says what TOUCHING that knob says, plus the position.
     *
     * announceTouch is the shared utterance — name, value, and "click to open"
     * for an opaque one — so the list and the grid describe a param identically.
     * Position is appended because a list has one and a grid does not.
     */
    function announceKnobRow(p, at) {
        const pg = p === undefined ? page() : p;
        const rows = knobRows(pg);
        const r = rows[at];
        if (!r) return;
        const meta = s.metaIndex ? s.metaIndex.getOrGuess(r.key) : null;
        let spoken = s.values[r.key];
        if (formatValue) {
            const resolved = formatValue(fullKey(r.key), spoken, "header");
            if (resolved !== null && resolved !== undefined) spoken = resolved;
        }
        const dec = s.decorations ? s.decorations[r.slot] : null;
        announce(`${announceTouch(meta, spoken, r.slot, dec)}, ${at + 1} of ${rows.length}`);
    }

    /** Move the row cursor. No write — the value is not touched by moving to it. */
    function stepKnobRow(p, delta) {
        const pg = p === undefined ? page() : p;
        if (!knobRows(pg).length) return false;
        const before = knobRowIndex(pg);
        setKnobRowIndex(pg, before + delta);
        const at = knobRowIndex(pg);
        if (at === before) return false;
        announceKnobRow(pg, at);
        return true;
    }

    /**
     * Editing: the jog IS the knob.
     *
     * Not "like" the knob — it calls onKnobTurn with the row's own slot, so the
     * acceleration curve, the enum seeding, the write throttle, the settle
     * window, the pending-write flush, the visible_if re-plan and the throttled
     * announcement are all the ones the grid turn uses, because they are the
     * same call. There is no second step function and no second write path to
     * keep in step with it.
     */
    function knobEditStep(p, delta) {
        const pg = p === undefined ? page() : p;
        const r = knobRows(pg)[knobRowIndex(pg)];
        if (!r) return false;
        return onKnobTurn(r.slot, delta > 0 ? 1 : -1) !== null;
    }

    /*
     * A menu page is INERT until you enter it.
     *
     * The first cut had the jog drive the list whenever a menu was on screen,
     * which quietly gave the jog two meanings depending on which page you were
     * on — an invisible mode — and then needed Shift as an escape from the trap
     * that created. This way the jog means ONE thing everywhere: it pages.
     *
     * A menu is simply a door at page scale. It wears the same brackets a
     * divable cell wears, it is entered with the same click, and it is left
     * with Back — the identical grammar one level up. Inert, it is also a
     * preview: you can read the actions while paging past without engaging.
     */
    /*
     * Which page kinds are DOORS: inert on arrival, entered with a click, left
     * with Back.
     *
     * A menu was the first. A preset browser is the second, and wants it more:
     * paging onto one used to hand the jog straight to the preset list, so
     * scrolling past a synth's presets on the way somewhere else LOADED every
     * preset it passed. The jog means one thing everywhere — it pages — until
     * you have said otherwise by clicking in.
     */
    /*
     * A knob page is the fourth, and ONLY when it is drawn as a list.
     *
     * On the grid there is nothing to enter: the jog pages, the eight knobs are
     * the controls, and a click dives the cell under your hand. Drawn as rows
     * there IS a cursor, and handing the jog to it on arrival would give the jog
     * two meanings depending on which page you were on — the invisible mode this
     * whole door rule exists to prevent, and the reason paging past a preset
     * browser no longer auditions every preset it crosses.
     *
     * Deciding it here rather than at each of the six call sites is the point:
     * menuEntered, enterMenu, exitMenu, goToPage's enterIfDoor and the bracket
     * frame all follow from the one answer.
     */
    function isDoor(p) {
        if (p && p.kind === PAGE_KNOBS) return knobsAsList(p);
        return !!(p && (p.kind === PAGE_MENU || p.kind === PAGE_PRESET
                        || p.kind === PAGE_ITEMS));
    }
    function menuEntered() {
        const p = page();
        return !!(isDoor(p) && s.menuEntered === p.name);
    }
    /** Enter the menu on this page. False when there is nothing to enter. */
    function enterMenu() {
        const p = page();
        if (!isDoor(p)) return false;
        if (p.kind === PAGE_MENU && !(p.entries || []).length) return false;
        if (p.kind === PAGE_ITEMS) {
            const st = itemsState(p);
            if (!st || !st.list.length) return false;   /* nothing to choose from */
        }
        s.menuEntered = p.name;
        if (p.kind === PAGE_KNOBS) {
            /* Entering hands over the ROW cursor, never the value: an arrival
             * writes nothing, the same rule a preset browser follows. */
            s.knobEditing = false;
            announceKnobRow(p, knobRowIndex(p));
            return true;
        }
        if (p.kind === PAGE_PRESET) {
            announce(`${p.name}, ${presetSpoken()}`);
            return true;
        }
        if (p.kind === PAGE_ITEMS) {
            const st = itemsState(p);
            const it = st && st.list[st.cursor];
            announce(it ? `${p.name}, ${it.label}, ${st.cursor + 1} of ${st.list.length}`
                        : `${p.name}, empty`);
            return true;
        }
        const e = menuEntry();
        if (e) announce(`${p.name}, ${e.label}${e.value ? ", " + e.value : ""}`);
        return true;
    }
    /** Leave the menu without activating anything. */
    function exitMenu() {
        if (!menuEntered()) return false;
        /* Back steps out ONE level. Editing a value is inside the list, so the
         * first Back gives the jog back to the row cursor and the second leaves
         * the list — otherwise a mis-click would drop you off the page with the
         * jog still in edit mode as far as the user could tell. */
        if (s.knobEditing) {
            s.knobEditing = false;
            announceKnobRow(page(), knobRowIndex(page()));
            return true;
        }
        s.menuEntered = null;
        announcePageChange();
        return true;
    }

    function openPicker() {
        s.pickerEntries = groupIndex(s.pages);
        if (!s.pickerEntries.length) return false;
        /* Start on the section you are already in. */
        let cur = 0;
        for (let i = 0; i < s.pickerEntries.length; i++) {
            if (s.pickerEntries[i].index <= s.pageIndex) cur = i;
        }
        s.pickerIndex = cur;
        s.pickerOpen = true;
        announce(`Sections, ${s.pickerEntries[cur].name}, ${cur + 1} of ${s.pickerEntries.length}`);
        return true;
    }

    function closePicker() {
        if (!s.pickerOpen) return false;
        s.pickerOpen = false;
        announcePageChange();
        return true;
    }

    /** Commit the highlighted section and return to the grid. */
    function pickerSelect() {
        if (!s.pickerOpen) return false;
        const entry = s.pickerEntries[s.pickerIndex];
        s.pickerOpen = false;
        /* Naming a section in the picker is choosing it, not paging past it —
         * reported for airwindows, where Presets and Jump To Category are both
         * sections and both arrived shut. Same rule as navigate_to. */
        if (entry) goToPage(entry.index, { enterIfDoor: true });
        return true;
    }

    /* Keyed on level+kind, not level alone: a level can carry more than one
     * page kind sharing one level key (braids' root is both the "Presets"
     * PAGE_PRESET browser and the "Main" PAGE_KNOBS grid) — a level-only key
     * let memory of one hijack a jump to the other. Picking "Presets" from
     * the section list landed back on "Main" because sectionMemory["root"]
     * held the knobs page and restoreSection only checked level. */
    function sectionKey(p) { return p ? `${p.level}\u0000${p.kind}` : null; }

    /* Remember where you were within the current section. */
    function rememberSection() {
        const p = page();
        const key = sectionKey(p);
        if (p && p.level && key) s.sectionMemory[key] = s.pageIndex;
    }

    /* Landing on a section: return to the sub-page last used there. */
    function restoreSection(index) {
        const p = s.pages[index];
        const key = sectionKey(p);
        if (!p || !p.level || !key) return index;
        const remembered = s.sectionMemory[key];
        if (remembered === undefined) return index;
        const rp = s.pages[remembered];
        return (rp && sectionKey(rp) === key) ? remembered : index;
    }

    /** Jog: pages. With shift: whole levels, skipping continuations. */
    function onJog(delta, { shift = false } = {}) {
        if (s.hintLines) dismissHint();
        if (s.pickerOpen) {
            const n = s.pickerEntries.length;
            if (!n) return s.pageIndex;
            const before = s.pickerIndex;
            s.pickerIndex = Math.max(0, Math.min(n - 1, s.pickerIndex + (delta > 0 ? 1 : -1)));
            if (s.pickerIndex !== before) {
                const e = s.pickerEntries[s.pickerIndex];
                announce(`${e.name}, ${s.pickerIndex + 1} of ${n}`);
            }
            return s.pageIndex;
        }
        /* On a menu page the jog belongs to the LIST, not to the page set —
         * the entries are what you are navigating. Shift still pages out, so
         * the menu is never a trap. */
        const mp = page();
        /* Entered knob list: the jog is the row cursor, or — once a row has
         * been opened for editing — the knob itself. Shift still pages out, so
         * the page set is never unreachable. */
        if (knobsAsList(mp) && menuEntered() && !shift) {
            if (s.knobEditing) knobEditStep(mp, delta);
            else stepKnobRow(mp, delta > 0 ? 1 : -1);
            return s.pageIndex;
        }
        /* Entered items page: the jog moves the highlight. Nothing is written
         * until you click, so scrolling a soundfont list is free. */
        if (mp && mp.kind === PAGE_ITEMS && menuEntered() && !shift) {
            stepItems(delta > 0 ? 1 : -1);
            return s.pageIndex;
        }
        /* Entered preset page: the jog is the browser. Shift still pages out,
         * so the page set is never unreachable — same escape a menu has. */
        if (mp && mp.kind === PAGE_PRESET && menuEntered() && !shift) {
            stepPreset(delta > 0 ? 1 : -1);
            return s.pageIndex;
        }
        if (mp && mp.kind === PAGE_MENU && menuEntered() && !shift) {
            const n = (mp.entries || []).length;
            if (n > 0) {
                const before = menuIndex(mp);
                setMenuIndex(mp, before + (delta > 0 ? 1 : -1));
                const now = menuIndex(mp);
                if (now !== before) {
                    const e = mp.entries[now];
                    announce(`${e.label}${e.value ? ", " + e.value : ""}, ${now + 1} of ${n}`);
                }
                return s.pageIndex;
            }
        }

        if (!s.pages.length || delta === 0) return s.pageIndex;
        const before = s.pageIndex;
        rememberSection();
        s.pageIndex = shift ? restoreSection(stepLevel(s.pages, s.pageIndex, delta))
                            : step(s.pages, s.pageIndex, delta);
        if (s.pageIndex !== before) {
            s.cursor = 0;
            s.touched = -1;
            s.turnClaimMs = 0;
            /* Leaving a page leaves the door it was. Without this, entering a
             * browser, Shift+jogging away and jogging BACK put you inside it
             * again without a click — the page had never been marked as left,
             * only navigated off. */
            s.menuEntered = null;
            announcePageChange();
        }
        return s.pageIndex;
    }

    /**
     * Jump straight to a page (from the index or group picker).
     *
     * `enterIfDoor` is the difference between arriving by PAGING and arriving
     * by CHOOSING. The jog is inert on a door until you click in, so that
     * paging past a preset browser cannot audition every preset it passes —
     * but a page you named, from a picker or by following a navigate_to, was
     * not passed, it was asked for. Needing a second gesture to make the first
     * one take effect is the bug; both of those callers opt in, and everything
     * that pages keeps the old rule.
     *
     * Deciding it HERE rather than at the call site is not tidiness: with
     * `remember` on, restoreSection can land you on a different page of the
     * section than the index you passed, so only this function knows what you
     * actually arrived at.
     *
     * The enter does the announcing when it happens — otherwise the reader
     * utters the item you chose, then the page name, then the entered list,
     * and only the last is news.
     */
    function goToPage(index, { remember = true, enterIfDoor = false } = {}) {
        /* Paging away cannot leave a menu entered — returning later would
         * silently hand the jog back to the list. (Page names are unique, so
         * this and "any index change" are the same rule; onJog carries the
         * equivalent clear.) */
        if (s.menuEntered && s.pages[index] && s.pages[index].name !== s.menuEntered) {
            s.menuEntered = null;
        }
        if (index === s.pageIndex) {
            /* Naming the page you are already on still means "open it". */
            if (enterIfDoor && isDoor(page()) && !menuEntered()) enterMenu();
            return s.pageIndex;
        }
        rememberSection();
        const target = Math.max(0, Math.min(s.pages.length - 1, index));
        s.pageIndex = remember ? restoreSection(target) : target;
        s.cursor = 0;
        s.touched = -1;
        s.turnClaimMs = 0;
        /* enterMenu refuses an empty list, and then nobody has spoken yet. */
        if (!(enterIfDoor && isDoor(page()) && enterMenu())) announcePageChange();
        return s.pageIndex;
    }

    /**
     * A physical knob moved. Applies the shared knob_engine so a value moves
     * identically here and in the list editor, writes through, and holds off
     * reads for that key until it settles.
     */
    function onKnobTurn(slot, direction, nowMs, { fine = false } = {}) {
        if (s.hintLines) dismissHint();
        const key = keyAt(slot);
        if (!key) return null;
        const meta = s.metaIndex.getOrGuess(key);
        /* A filepath or canvas cannot be turned — it opens. Swallow the motion
         * rather than writing nonsense into it. */
        if (!isTurnable(meta)) return null;

        const t = nowMs === undefined ? now() : nowMs;

        /* Turning claims the header: "last touched or MOVED" is the one you are
         * working on, and a knob can be turned without the capacitive touch
         * ever registering. It does not join touchOrder — nothing is being
         * held — so it stops leading the header as soon as a held knob does,
         * and if nothing is held it expires on its own (TURN_CLAIM_MS): there
         * is no release event coming for a knob no finger registered on. */
        if (!s.touchOrder.length) {
            s.touched = slot;
            s.turnClaimMs = t;
        } else if (s.touchOrder.indexOf(slot) >= 0) {
            s.touched = slot;
            s.turnClaimMs = 0;
        }

        /* ONE knob model, whatever the layout. There used to be two, and this
         * branch picked between them by layout -- a knob that behaves
         * differently depending on which screen you touched it from is a bug,
         * not a layout choice. See shared/knob_engine.mjs. */
        let st = s.knobStates[key];
        if (!st) {
            /* Turning a knob the read cursor has not reached yet is the one
             * case that reads here — and since that read IS from the device,
             * it is also allowed to settle the enum wire format, for a page
             * whose first gesture beats its first read. */
            let raw = s.values[key];
            if (raw === undefined) {
                raw = getParam(fullKey(key));
                learnEnumWireFormat(meta, raw);
            }
            /* An enum reported as a NAME is still an index to the engine —
             * Number("major") is NaN, which used to seed every such knob at
             * option 0 and jump the value on the first detent. */
            let start;
            if (meta.kind === KIND_ENUM) {
                const idx = enumIndexOf(meta, raw);
                start = idx >= 0 ? idx : 0;
            } else {
                /* A plain number keeps its sign — clamping to >= 0 here would
                 * seed a bipolar param (transpose, pan) at 0 instead of where
                 * it is. */
                const num = Number(raw);
                start = isFinite(num) ? num : 0;
            }
            st = s.knobStates[key] = knobInit(start);
        }

        /*
         * Fine adjust: Elektron's [FUNC]+encoder. Holding shift already reveals
         * every value, so precision mode and "show me the numbers" are the same
         * gesture -- which is what you want when chasing a value.
         *
         * Passed through for EVERY type. It used to be gated to floats here on
         * the reasoning that an int moves in whole units and an enum in whole
         * options, so there is nothing finer. That is wrong on both counts once
         * the step is normalised to the range: a 0..20000 int moves 100 at a
         * time coarse, and an enum is gated at 4 detents per option, so both
         * have a finer setting to give and shift did nothing at all on them.
         */
        const value = knobStep(st, meta, direction, t, fine);
        const wire = formatParamForSet(value, meta);

        s.values[key] = wire;
        s.settleUntil[key] = s.tickCount + SETTLE_TICKS;
        /* Throttled — see SETPARAM_THROTTLE_MS. A miss is never lost: it is
         * left in pendingWrite for tick() to flush once the window passes,
         * and onKnobTouch(false) flushes immediately on release. */
        const lastWrite = s.lastWriteMs[key] || 0;
        if (t - lastWrite >= SETPARAM_THROTTLE_MS) {
            s.lastWriteMs[key] = t;
            delete s.pendingWrite[key];
            setParam(fullKey(key), wire);
        replanIfCondition(key);
        } else {
            s.pendingWrite[key] = wire;
        }
        /* Throttled — see ANNOUNCE_THROTTLE_MS. A continuous fast turn still
         * announces regularly, just not once per raw MIDI detent. */
        const lastAnnounce = s.lastAnnounceMs[key] || 0;
        if (t - lastAnnounce >= ANNOUNCE_THROTTLE_MS) {
            s.lastAnnounceMs[key] = t;
            announce(announceTurn(meta, wire));
        }
        return wire;
    }

    /**
     * Set an enum to a chosen OPTION INDEX, as an option picker does.
     *
     * The same tail as onKnobTurn — format, cache, settle, write, re-plan —
     * minus the knob engine, because a picker has no running numeric state to
     * carry: it hands over an index straight out of a list.
     *
     * Two details that are not decoration:
     *
     *   the write goes through enumWireValue, so a plugin that only accepts
     *   option NAMES gets a name. Writing String(index) here is exactly the
     *   chord bug, and a picker is where it is most tempting.
     *
     *   the knob state for this key is DROPPED. It was seeded from the value
     *   before the picker ran, so the first detent afterwards would step from
     *   there and snap the value back to where the user had just moved it away
     *   from. It is re-seeded from s.values on the next turn.
     *
     * The wire format is NOT learned here: `s.values` holds writes the grid
     * made as well as reads from the device, and learning from our own write
     * is a verdict that makes itself true. Detection belongs on the read
     * cursor and on the picker-open read (see onClick).
     */
    function commitEnum(key, index) {
        const meta = key ? s.metaIndex.getOrGuess(key) : null;
        if (!meta || meta.kind !== KIND_ENUM) return null;
        let i = Math.round(Number(index));
        if (!isFinite(i)) return null;
        const n = Array.isArray(meta.options) ? meta.options.length : 0;
        if (n > 0) i = Math.max(0, Math.min(n - 1, i));
        const wire = enumWireValue(meta, i);
        s.values[key] = wire;
        s.settleUntil[key] = s.tickCount + SETTLE_TICKS;
        s.lastWriteMs[key] = now();
        delete s.pendingWrite[key];
        delete s.knobStates[key];
        setParam(fullKey(key), wire);
        replanIfCondition(key);
        return wire;
    }

    /*
     * A key that gates visibility has changed — re-plan, because the params it
     * hides or reveals are not otherwise reachable.
     *
     * Called from the READ cursor and from every WRITE. Read-only was not
     * enough: turning the gating knob updates s.values immediately, so when the
     * cursor next reads that key nothing has "changed" and the re-plan never
     * fired. Switching an LFO to Sync left the Hz cell on screen — the value had
     * moved and the page had not.
     */
    /*
     * Re-plan from the cached contract with the current mode.
     *
     * Same operation replanIfCondition performs for a visibility change -- the
     * hierarchy and chain_params are already in hand, so this costs no device
     * reads -- but unconditional, because a mode changes the walk ROOT and
     * therefore every page, not just which of them are visible.
     */
    function replanForMode() {
        if (!s.hierarchy) return;
        const planned = planPages({
            hierarchy: s.hierarchy, chainParams: s.chainParams,
            mode: s.lastLoadOpts && s.lastLoadOpts.mode,
            visible: s.lastLoadOpts && s.lastLoadOpts.visible,
            trailingMenus: trailingMenus(),
        });
        if (!planned.pages.length) return;   /* never plan from nothing */
        s.pages = planned.pages;
        s.fingerprint = planned.fingerprint;
        s.conditionKeys = planned.conditionKeys || new Set();
        s.values = Object.create(null);
        s.knobStates = Object.create(null);
        s.cursor = 0;
        s.pageIndex = firstGrid(s.pages);
    }

    /*
     * Re-evaluate ONLY the trailing pages, in place.
     *
     * Not a re-plan: replanForMode resets pageIndex to firstGrid and
     * replanIfCondition reanchors, and both would move you off the page you
     * are standing on — which is exactly the page whose rows just changed,
     * because you are the one who changed them (pressing Save on a "User
     * Presets" page). Costs no device reads: the rows come from the host's
     * own state, not from the module.
     *
     * Reuses buildTrailingPages/makeClaimer rather than re-deriving the
     * trailing pages by hand, so the entry transform and the name-collision
     * loop stay defined exactly once, in page_plan.mjs.
     */
    function refreshTrailing() {
        /* Mirrors replanForMode's own guard: with no hierarchy there is
         * nothing this component declared to rebuild against, and s.pages
         * defaults to [] — without this a call before load() would replace
         * that empty set with a lone trailing page and no non-trailing pages
         * under it. */
        if (!s.hierarchy) return;
        const nonTrailing = s.pages.filter((p) => !p.trailing);
        const claim = makeClaimer(new Set(nonTrailing.map((p) => p.name)));
        /* built.warnings is dropped here, same as at every other plan site in
         * this file — page_controller.mjs never surfaces planPages' warnings
         * array; only validate_contract.mjs and preview.mjs consume it. */
        const built = buildTrailingPages(trailingMenus(), claim);
        s.pages = nonTrailing.concat(built.pages);
        if (s.pageIndex >= s.pages.length) s.pageIndex = Math.max(0, s.pages.length - 1);
    }

    function replanIfCondition(key) {
        if (!s.conditionKeys.has(key)) return;
        const oldPages = s.pages, oldIndex = s.pageIndex;
        const planned = planPages({
            hierarchy: s.hierarchy, chainParams: s.chainParams,
            mode: s.lastLoadOpts && s.lastLoadOpts.mode,
            visible: s.lastLoadOpts && s.lastLoadOpts.visible,
            trailingMenus: trailingMenus(),
        });
        if (planned.pages.length !== oldPages.length ||
            planned.pages.some((p, i) => (p.keys || []).join() !== ((oldPages[i] || {}).keys || []).join())) {
            s.pages = planned.pages;
            s.conditionKeys = planned.conditionKeys || new Set();
            s.pageIndex = reanchor(oldPages, oldIndex, s.pages);
            s.cursor = 0;
        }
    }

    /**
     * Forget every held knob.
     *
     * Called when the grid hands off to another screen: the note-off for the
     * knob you were holding goes to THAT screen, never back to the grid, so
     * without this the cell stays highlighted for the rest of the session.
     * Holding Target and clicking it is exactly that sequence.
     */
    function clearTouch() {
        s.touchOrder.length = 0;
        s.touched = -1;
        s.turnClaimMs = 0;
    }

    /*
     * Capacitive touch. Down announces the full name and value.
     *
     * TOUCH IS A SET, not a slot. Hands have more than one finger: hold one
     * knob, touch a second, release the second, and the first is still held —
     * but a single `touched` index had already been overwritten and then
     * cleared, so the knob under your finger stopped being highlighted.
     *
     * `touchOrder` keeps every knob currently down, in the order they were
     * touched. Every one of them highlights; the header follows the LAST one
     * touched or turned, which is the one you are actually working on.
     */
    function onKnobTouch(slot, down) {
        if (s.hintLines) dismissHint();
        /* Reaching for a knob is an unambiguous "I want the grid", so it
         * dismisses the picker rather than leaving you in a modal you have to
         * back out of first. */
        if (down && s.pickerOpen) closePicker();
        if (!down) {
            const at = s.touchOrder.indexOf(slot);
            if (at >= 0) s.touchOrder.splice(at, 1);
            /* The header falls back to whatever is still held, not to nothing. */
            s.touched = s.touchOrder.length ? s.touchOrder[s.touchOrder.length - 1] : -1;
            /* A real hold outranks and cancels any pending turn-claim. */
            s.turnClaimMs = 0;
            /* Release flushes immediately rather than waiting out
             * SETPARAM_THROTTLE_MS — the hand has stopped, so there is no
             * more flooding to protect against, and the settled value should
             * land on the device the instant you let go, not up to 20ms
             * later. */
            const key = keyAt(slot);
            if (key && s.pendingWrite[key] !== undefined) {
                setParam(fullKey(key), s.pendingWrite[key]);
                replanIfCondition(key);
                s.lastWriteMs[key] = now();
                delete s.pendingWrite[key];
            }
            return;
        }
        if (s.touchOrder.indexOf(slot) < 0) s.touchOrder.push(slot);
        s.touched = slot;
        s.turnClaimMs = 0;
        const key = keyAt(slot);
        const meta = metaAt(slot);
        const dec = s.decorations ? s.decorations[slot] : null;
        /* Whatever the header is about to show is what gets spoken — a routing
         * read out as "fx1" is no more use by ear than it is by eye. */
        let spoken = key ? s.values[key] : null;
        if (formatValue && key) {
            const resolved = formatValue(fullKey(key), spoken, "header");
            if (resolved !== null && resolved !== undefined) spoken = resolved;
        }
        announce(announceTouch(meta, spoken, slot, dec));
    }

    /**
     * Click on a knob's cell. A DIVABLE param (filepath, canvas, string, and a
     * ranged wav_position) asks the caller to open the editor the list view
     * already has. The controller never opens it itself — that screen belongs to
     * the host.
     *
     * Gated on meta.divable rather than kind === OPAQUE so a wav_position, which
     * is a turnable number, still opens its waveform editor on click while the
     * knob keeps driving it.
     */
    function onClick(slot) {
        /* A menu page has no knobs, so the click has exactly one meaning:
         * activate the highlighted entry. The controller does not perform it —
         * the host owns whatever Save or Knob Mapping means, same rule that
         * keeps it out of the editors. */
        const mp = page();
        /*
         * A preset page: the first click goes IN, the second says done.
         *
         * Done means the first grid page, not "nothing". You came here to
         * choose a sound and the browser loads as you scroll, so by the time
         * you click there is nothing left to commit — what you want next is
         * the knobs for the preset you just landed on. Leaving you in the
         * browser makes the click do nothing and the page feel like somewhere
         * you are stuck, with only Back to get out and Back only ever going
         * backwards.
         *
         * Back still steps out in place, for when you were only looking.
         */
        if (mp && mp.kind === PAGE_ITEMS) {
            if (!menuEntered()) { enterMenu(); return null; }
            commitItem();
            return null;
        }
        if (mp && mp.kind === PAGE_PRESET) {
            if (!menuEntered()) { enterMenu(); return null; }
            s.menuEntered = null;
            const grid = firstGrid(s.pages);
            if (grid >= 0 && grid !== s.pageIndex) goToPage(grid, { remember: false });
            else announcePageChange();
            return null;
        }
        if (mp && mp.kind === PAGE_MENU) {
            /* First click enters the menu; the next activates the entry under
             * the cursor. The same two-step a divable cell has (hold, then
             * click) and the picker has (open, then choose). */
            if (!menuEntered()) { enterMenu(); return null; }
            const e = menuEntry();
            if (!e) return null;
            s.pending = { action: "menu", entry: e, level: mp.level };
            return s.pending;
        }
        /*
         * A knob page drawn as a LIST. The click means exactly what it means on
         * the grid — it just takes its param from the CURSOR instead of from
         * the knob under your hand, because on this layout there is no hand on a
         * knob to take it from.
         *
         * So the ladder below is not a second interaction model:
         *
         *   shut          click opens the list (a door, see isDoor)
         *   a TRIGGER     fires, exactly as the grid's cell does
         *   a DIVABLE     opens its editor or its enum picker, same pending
         *                 intent, same options payload, so the host opens the
         *                 same screen from either layout
         *   anything else turnable hands the jog to the value, which is the one
         *                 thing the grid does with a physical knob and a list
         *                 has to say some other way
         *
         * An OPAQUE row therefore has NO jog behaviour at all, which is the same
         * answer the grid gives: isTurnable is false for it, onKnobTurn already
         * swallows the motion, and the click that opens its editor is the whole
         * interaction. It is not made turnable here to give the jog something to
         * do — inventing an interaction for one layout is how the two surfaces
         * would come apart.
         */
        if (knobsAsList(mp)) {
            if (!menuEntered()) { enterMenu(); return null; }
            const r = knobRows(mp)[knobRowIndex(mp)];
            if (!r) return null;
            if (s.knobEditing) {
                /* Done. Values are written as they move, so there is nothing to
                 * commit — this only gives the jog back to the row cursor. */
                s.knobEditing = false;
                announceKnobRow(mp, knobRowIndex(mp));
                return null;
            }
            const rmeta = s.metaIndex ? s.metaIndex.getOrGuess(r.key) : null;
            if (rmeta && !rmeta.writeOnly && !rmeta.divable && isTurnable(rmeta)) {
                s.knobEditing = true;
                announceKnobRow(mp, knobRowIndex(mp));
                return null;
            }
            /* Trigger or divable: fall through to the grid's own handling with
             * the cursor's slot. */
            slot = r.slot;
        }

        const key = keyAt(slot);
        const meta = metaAt(slot);
        if (!key || !meta) return null;

        /*
         * A TRIGGER fires. It is not a value to open or scrub — the module
         * reports a constant and acts on the write, so a click is the whole
         * interaction.
         *
         * The wire value that fires it is the module's business
         * (["idle","trigger"], ["—","Rnd!"], ["Play","Save"] are all in the
         * fleet), so option 1 goes out through the ordinary enum wire, which
         * speaks whichever convention that module uses. Writing a bare "1"
         * here is exactly the bug that makes euclidrum randomise a kit when
         * asked to do nothing.
         */
        if (meta.writeOnly) {
            if (meta.kind === KIND_ENUM && Array.isArray(meta.options) && meta.options.length > 1) {
                setParam(fullKey(key), enumWireValue(meta, 1));
                announce(`${meta.label}, ${meta.options[1]}`);
            } else {
                setParam(fullKey(key), "1");
                announce(`${meta.label}`);
            }
            /*
             * Stamp the fire time so the bang can flash. OVERWRITING is the
             * point: clicking again mid-flash restarts the animation from the
             * beginning rather than being swallowed by the one in progress, so
             * two clicks look like two events. The renderer derives everything
             * from this one number, so there is no animation state to clear.
             */
            /*
             * APPEND, do not overwrite. A press must not cancel the bursts
             * already travelling — a fast double-tap should throw two rings,
             * not restart one. Trimmed to what can still be on screen, so the
             * list cannot grow.
             */
            const t = now();
            const prev = s.triggerFiredAt[key] || [];
            s.triggerFiredAt[key] = prev
                .filter((p) => t - p < TRIGGER_BURST_KEEP_MS)
                .slice(-TRIGGER_BURST_MAX + 1)
                .concat(t);
            return null;
        }

        if (!meta.divable) return null;
        s.pending = { action: "open", key, fullKey: fullKey(key), meta };
        /*
         * An enum opens a list of its OPTIONS, so the intent carries the list
         * and where in it we currently are — the host should not have to spend
         * an IPC read to find out what the cursor already knows.
         *
         * The read below is the exception, and it is also the only honest place
         * left to settle the wire format for a page whose first gesture beats
         * its first read: it comes from the DEVICE. Learning from s.values
         * would risk learning from a value the grid itself wrote.
         */
        if (meta.kind === KIND_ENUM && Array.isArray(meta.options)) {
            let raw = s.values[key];
            if (raw === undefined) {
                raw = getParam(fullKey(key));
                learnEnumWireFormat(meta, raw);
            }
            const idx = enumIndexOf(meta, raw);
            s.pending.options = meta.options.slice();
            s.pending.index = idx >= 0 ? idx : 0;
        }
        return s.pending;
    }

    function takePending() {
        const p = s.pending;
        s.pending = null;
        return p;
    }

    /* --------------------------------------------------------- presentation */

    /** Arm the first-run hint. Ignored once it has been shown and dismissed. */
    function showHint(lines, title) {
        if (s.hintShown) return false;
        s.hintLines = { lines, title };
        return true;
    }

    function dismissHint() {
        if (!s.hintLines) return false;
        s.hintLines = null;
        s.hintShown = true;
        return true;
    }

    /**
     * Re-read the live value of up to MOD_FAST_READS_PER_TICK modulated keys.
     *
     * `values` stays the BASE — what the user dialled in and what a turn edits
     * — and these are the effective values a source is currently driving the
     * param to, drawn as a dot on the arc. Keeping them apart is the whole
     * point: with the pointer chasing an LFO you cannot see what you set.
     *
     * Skips a key that is being turned, for the same reason the value cursor
     * does (`settleUntil`): a read issued before the turn lands after it.
     */
    function refreshModulatedValues(p) {
        const modKeys = [];
        for (const k of p.keys) {
            if (k && s.modCache[k]) modKeys.push(k);
        }
        if (!modKeys.length) {
            /* Nothing modulated: drop stale dots rather than leave them frozen
             * on the arc after a routing is removed. */
            if (s.modCursor !== 0) s.modCursor = 0;
            for (const k in s.modValues) delete s.modValues[k];
            return;
        }
        const n = Math.min(MOD_FAST_READS_PER_TICK, modKeys.length);
        for (let i = 0; i < n; i++) {
            const key = modKeys[(s.modCursor + i) % modKeys.length];
            /* Deliberately NOT gated on settleUntil, unlike the value cursor.
             * That gate exists because a stale read of the BASE lands after a
             * turn and drags the knob backwards — a write-back race. There is
             * no such race here: the UI never writes the effective value, it
             * only displays it. Gating it meant the dot froze for the whole
             * time you were turning the knob, which is exactly when you most
             * want to see where modulation is putting the param. */
            const v = getParam(fullKey(key));
            if (v !== null && v !== undefined) s.modValues[key] = v;
        }
        s.modCursor = (s.modCursor + n) % modKeys.length;
        /* A key that stopped being modulated keeps no dot. */
        for (const k in s.modValues) {
            if (!s.modCache[k]) delete s.modValues[k];
        }
    }

    function setLayout(layout) { s.layout = layout; }
    function setReveal(on) { s.revealValues = !!on; }
    function setDecorations(d) { s.decorations = d || null; }

    /* Movy layout is a whole separate renderer (its own fixed-geometry header
     * and knob grid, not a `layout` value render_page.mjs understands — see
     * render_page_movy.mjs), so it does not take decorations (a sequencer's
     * per-slot p-locks) or an embedding `rect`: it draws its own header full
     * width, the way Movy itself always does. Anything using those keeps
     * LAYOUT_DIAL/LAYOUT_BAR — see setLayout. */
    /*
     * `footer` is [key, action] hint pairs, most important first, supplied by
     * the CALLER — the gestures belong to whoever owns the input mapping, same
     * reason the first-run hint panel's text does. Movy layout only: the
     * dial/bar grid has no footer band reserved and would draw over its last
     * label row.
     */
    /*
     * A knob page as five rows, in the page chrome every other kind wears.
     *
     * Nothing here is new geometry: MENU_LIST_* is the rect the menu, items and
     * section-picker pages already occupy, and MENU_FRAME_* is the bracket frame
     * already drawn around an un-entered one. Two lists in one rectangle that
     * were merely SIMILAR is the drift this file's drawPageChromeList comment
     * spends twenty lines on; a second rect for this layout would be that again.
     *
     * The BRACKETS are load-bearing rather than decorative: they are what says
     * this page is something you can go into, and they are the same mark a
     * divable cell and an un-entered menu wear.
     */
    function drawKnobsAsList(ctx, title, footer) {
        const mp = page();
        drawHeaderMovy(ctx, title || "", pageLabel(mp), false);
        drawBankBar(ctx, s.pageIndex | 0, Math.max(1, s.pages.length), pageGroups());
        const bottom = footer ? RULE_Y : 64;
        const entered = menuEntered();
        drawPageChromeList(ctx,
            { x: MENU_LIST_X, y: MENU_LIST_Y, w: MENU_LIST_W, h: bottom - MENU_LIST_Y },
            knobListEntries(mp),
            entered ? knobRowIndex(mp) : -1,
            { editMode: entered && s.knobEditing });
        if (!entered) {
            drawBrackets(ctx, MENU_FRAME_X, MENU_FRAME_Y, MENU_FRAME_W,
                         bottom - MENU_FRAME_Y - MENU_FRAME_BOTTOM_INSET,
                         MENU_BRACKET_LEN);
        }
        if (footer) drawFooter(ctx, footer);
    }

    function render(ctx, { title, rect, footer } = {}) {
        /* LAYOUT_LIST is LAYOUT_MOVY with one page kind arranged differently, so
         * it takes the same branch: the header, bank bar, footer, section
         * picker, menu, items and preset pages are all literally the same draws.
         * Only the knob page forks, and only at the last step. */
        if (s.layout === LAYOUT_MOVY || s.layout === LAYOUT_LIST) {
            const drawGrid = () => {
            if (knobsAsList()) { drawKnobsAsList(ctx, title, footer); return; }
            renderPageMovy(ctx, {
                page: page(), metaIndex: s.metaIndex, values: s.values,
                title: title || "", pageIndex: s.pageIndex, pageCount: s.pages.length,
                touched: s.hintLines ? -1 : s.touched,
                displayFor: formatValue
                    ? (key, raw, surface) => formatValue(fullKey(key), raw, surface)
                    : null,
                /* Every knob under a finger inverts its label, not just the one
                 * the header is following. */
                touchedSlots: s.hintLines ? [] : s.touchOrder,
                modulated: (key) => !!s.modCache[key],
                modValues: s.modValues,
                pageGroups: pageGroups(),
                pageLabel: pageLabel(),
                viz: vizEnabled ? vizGroups() : [],
                /*
                 * The trigger button's press animation. Both of these have to
                 * come from here: the renderer is pure and reads the clock off
                 * `o`, so without them every button draws in its idle phase
                 * forever -- which is exactly what shipped, because the test
                 * handed the renderer both directly and so only ever proved
                 * the renderer, never the wiring.
                 */
                triggerFiredAt: s.triggerFiredAt,
                nowMs: now(),
                footer,
            });
            };
            if (s.hintLines) {
                drawGrid();
                renderHint(ctx, { rect, lines: s.hintLines.lines, title: s.hintLines.title });
                return;
            }
            if (s.pickerOpen) {
                /*
                 * The section picker wears the PAGE chrome, not a chrome of its
                 * own. It used to draw its own taller header, so the one screen
                 * that is explicitly about navigating pages was the one screen
                 * that did not look like a page. Same header band, same bank
                 * bar, same list rect and same five rows as a menu page — the
                 * only difference is what the list holds.
                 */
                const pbottom = footer ? RULE_Y : 64;
                drawHeaderMovy(ctx, title || "", "SECTIONS", false);
                drawBankBar(ctx, s.pageIndex | 0, Math.max(1, s.pages.length), pageGroups());
                drawPageChromeList(ctx,
                    { x: MENU_LIST_X, y: MENU_LIST_Y,
                      w: MENU_LIST_W, h: pbottom - MENU_LIST_Y },
                    s.pickerEntries, s.pickerIndex);
                if (footer) drawFooter(ctx, footer);
                return;
            }
            const mp = page();
            if (mp && mp.kind === PAGE_ITEMS) {
                /* A real list, so it draws like a menu page: same chrome, same
                 * five rows, same rect. Inert it highlights nothing — the page
                 * is something you can go INTO, not something you are in. */
                drawHeaderMovy(ctx, title || "", mp.name, false);
                drawBankBar(ctx, s.pageIndex | 0, Math.max(1, s.pages.length), pageGroups());
                const ibottom = footer ? RULE_Y : 64;
                const ist = itemsState(mp) || { list: [], cursor: 0, current: -1 };
                const entered = menuEntered();
                drawPageChromeList(ctx,
                    { x: MENU_LIST_X, y: MENU_LIST_Y,
                      w: MENU_LIST_W, h: ibottom - MENU_LIST_Y },
                    ist.list.length
                        ? ist.list.map((it) => ({
                            name: it.label,
                            /* The one in force marks itself where a menu page
                             * puts a value, same as the module picker. */
                            value: it.index === ist.current ? "*" : "",
                          }))
                        : [{ name: "(none)", value: "" }],
                    entered ? ist.cursor : -1);
                if (!entered) {
                    drawBrackets(ctx, MENU_FRAME_X, MENU_FRAME_Y, MENU_FRAME_W,
                                 ibottom - MENU_FRAME_Y - MENU_FRAME_BOTTOM_INSET,
                                 MENU_BRACKET_LEN);
                }
                if (footer) drawFooter(ctx, footer);
                return;
            }
            if (mp && mp.kind === PAGE_PRESET) {
                /* Same chrome as a grid page — module name, page name, bank
                 * bar and footer all stay put, so the preset browser reads as
                 * one of this module's pages rather than as somewhere else.
                 * That is the whole point: it used to eject into the list
                 * editor, which looks nothing like this. */
                drawHeaderMovy(ctx, title || "", mp.name, false);
                drawBankBar(ctx, s.pageIndex | 0, Math.max(1, s.pages.length), pageGroups());
                const pbottom = footer ? RULE_Y : 64;
                const prect = { x: MENU_FRAME_X, y: MENU_FRAME_Y,
                                w: MENU_FRAME_W, h: pbottom - MENU_FRAME_Y - MENU_FRAME_BOTTOM_INSET };
                const pst = presetState(mp) || {};
                drawPresetBody(ctx, prect, {
                    name: pst.name, index: pst.index, count: pst.count,
                    entered: menuEntered(),
                });
                /* Inert: it wears the same brackets a divable cell and an
                 * un-entered menu wear, because it is the same offer. */
                if (!menuEntered()) {
                    drawBrackets(ctx, MENU_FRAME_X, MENU_FRAME_Y, MENU_FRAME_W,
                                 pbottom - MENU_FRAME_Y - MENU_FRAME_BOTTOM_INSET,
                                 MENU_BRACKET_LEN);
                }
                if (footer) drawFooter(ctx, footer);
                return;
            }
            if (mp && mp.kind === PAGE_MENU) {
                /* Same chrome as a grid page — the module name, the page name
                 * and the bank bar all stay put, so a menu reads as one of this
                 * module's pages rather than as somewhere else. header:false
                 * because that header is already drawn. */
                drawHeaderMovy(ctx, title || "", mp.name, false);
                drawBankBar(ctx, s.pageIndex | 0, Math.max(1, s.pages.length), pageGroups());
                const bottom = footer ? RULE_Y : 64;
                const entered = menuEntered();
                /*
                 * ONE list rect for both states. Shrinking it to make room for
                 * the brackets cost a row (4 vs 5 everywhere else) and made the
                 * rows jump as you entered. Instead the list is inset far enough
                 * that the brackets sit OUTSIDE it: the top arm lands on row 8
                 * and the bottom on row 54, both clear of the row fills
                 * (9..53), and the side arms clear the text at x+2 and the
                 * right-aligned values at x+w-2.
                 */
                const listRect = { x: MENU_LIST_X, y: MENU_LIST_Y,
                                   w: MENU_LIST_W, h: bottom - MENU_LIST_Y };
                drawPageChromeList(ctx, listRect,
                    (mp.entries || []).map((e) => ({ name: e.label, value: e.value })),
                    /* Inert: nothing is highlighted, because nothing is selected
                     * yet — the page is something you can go INTO, not something
                     * you are already in. */
                    entered ? menuIndex(mp) : -1);
                if (!entered) {
                    drawBrackets(ctx, MENU_FRAME_X, MENU_FRAME_Y, MENU_FRAME_W,
                                 bottom - MENU_FRAME_Y - MENU_FRAME_BOTTOM_INSET,
                                 MENU_BRACKET_LEN);
                }
                if (footer) drawFooter(ctx, footer);
                return;
            }
            drawGrid();
            return;
        }

        if (s.hintLines) {
            renderPage(ctx, {
                page: page(), metaIndex: s.metaIndex, values: s.values,
                title: title || "", pageIndex: s.pageIndex, pageCount: s.pages.length,
                touched: -1, layout: s.layout, rect,
            });
            renderHint(ctx, { rect, lines: s.hintLines.lines, title: s.hintLines.title });
            return;
        }
        if (s.pickerOpen) {
            renderPicker(ctx, { rect, entries: s.pickerEntries, index: s.pickerIndex, title: "Sections" });
            return;
        }
        renderPage(ctx, {
            page: page(), metaIndex: s.metaIndex, values: s.values,
            title: title || "", pageIndex: s.pageIndex, pageCount: s.pages.length,
            touched: s.touched, decorations: s.decorations,
            layout: s.layout, revealValues: s.revealValues, rect,
            modulated: (key) => !!s.modCache[key],
            /* Section ids for the page rule, so it groups the way Shift+jog
             * navigates. Cached — it only changes when the page set does. */
            pageGroups: pageGroups(),
            /* A sequencer's parameter-lock decorations are per SLOT; a
             * graphic replacing several slots with one picture would hide
             * which of them is locked, so graphics stand down while
             * decorations are active. */
            viz: (vizEnabled && !s.decorations) ? vizGroups() : [],
        });
    }

    let vizCache = null;
    function vizGroups() {
        const p = page();
        if (!p || p.kind !== PAGE_KNOBS || !s.metaIndex) return [];
        const cacheKey = `${s.fingerprint}#${s.pageIndex}`;
        if (vizCache && vizCache.key === cacheKey) return vizCache.groups;
        const { groups } = resolveViz({ keys: p.keys, metaIndex: s.metaIndex, overrides: vizOverrides });
        vizCache = { key: cacheKey, groups };
        return groups;
    }

    /** Read the current page aloud — the gesture that replaces a glance. */
    function announceContents() {
        announce(announcePageContents(page(), s.metaIndex, s.values, s.decorations));
    }

    let groupCache = null;
    function pageGroups() {
        if (groupCache && groupCache.fp === s.fingerprint) return groupCache.groups;
        const groups = s.pages.map((p) => (p.level === null || p.level === undefined) ? p.kind : p.level);
        groupCache = { fp: s.fingerprint, groups };
        return groups;
    }

    function announcePageChange() {
        announce(announcePage(page(), s.pageIndex, s.pages.length, pageLabel()));
    }

    return {
        load, reloadIfChanged, tick, refreshTrailing,
        /* For a selection made OUTSIDE the controller — the list editor drives
         * the same modules through its own preset browser and has the same
         * race. Books the settle; costs nothing until it comes due. */
        selectionChanged: armContractSettle,
        onJog, goToPage, restorePage, pageLabel, onKnobTurn, onKnobTouch, onClick, takePending, commitEnum,
        openPicker, closePicker, pickerSelect, showHint, dismissHint,
        menuEntry, menuIndex: () => menuIndex(page()),
        menuEntered, enterMenu, exitMenu, clearTouch,
        /* IS THIS PAGE A DOOR — one definition, exported because page_input.mjs
         * needs the same answer to route a plain click.
         *
         * It used to keep its own copy, spelled as a literal list of kinds
         * ("menu" || "preset" || "items"). When PAGE_KNOBS became a door in the
         * list layout that copy was not updated, so clicking a knobs-as-list
         * page fell through to the no-knob-held branch and opened the SECTION
         * PICKER instead of entering the list — the page was unusable, and
         * nothing failed. Ask the controller; do not restate the kinds. */
        isDoor: (p) => isDoor(p === undefined ? page() : p),
        /* The knob page as rows (LAYOUT_LIST). Read-only views, for the host's
         * footer hints and for tests — the rows carry the knob SLOT each came
         * from, which is what every edit is dispatched through. */
        knobRows: () => knobRows(),
        knobRowIndex: () => knobRowIndex(),
        knobListEntries: () => knobListEntries(),
        get knobEditing() { return s.knobEditing; },
        get pickerOpen() { return s.pickerOpen; },
        get pickerEntries() { return s.pickerEntries; },
        get pickerIndex() { return s.pickerIndex; },
        setLayout, setReveal, setDecorations, render, announceContents,
        get state() { return s; },
        get page() { return page(); },
        get pages() { return s.pages; },
        get pageIndex() { return s.pageIndex; },
        /** The loaded preset's name, once the cursor has read it. */
        get presetName() { return s.presetName; },
        /** This key's modulation flag as of the last time the cursor reached
         *  it. Read-only view of the cache the renderer uses — the injected
         *  isModulated is deliberately NOT called during a draw. */
        isModulatedCached: (key) => !!s.modCache[key],
        get metaIndex() { return s.metaIndex; },
        /** True while `<prefix>:ui_hierarchy` could not be READ. The page set,
         *  if any, is the previous one — nothing here was planned from the
         *  failure. */
        get contractUnresolved() { return s.contractUnresolved; },
        get triggerFiredAt() { return s.triggerFiredAt; },
        keyAt, metaAt,
        jumpIndex: () => jumpIndex(s.pages),
        groupIndex: () => groupIndex(s.pages),
    };
}

function parse(raw) {
    if (!raw) return null;
    try { return JSON.parse(raw); } catch { return null; }
}
