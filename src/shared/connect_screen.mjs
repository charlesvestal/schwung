/*
 * connect_screen.mjs — "point your phone here", as one picture.
 *
 * Schwung Manager (http://<device ip>:7700) is the single install/update path
 * and the file browser, and until now the device never said its own address out
 * loud: every pointer screen printed `move.local:7700`, which is an mDNS name
 * that works on Apple hardware, sometimes on Android, and not at all on a
 * network that filters multicast. Somebody standing in front of the Move with a
 * phone had no way to find out the number.
 *
 * So this draws the number AND a QR of the URL, and it is the ONE drawing for
 * that: the Global Settings Connect entry and every `[Get more...]` row in the
 * module pickers arrive at the same screen. Two pointer screens would drift.
 *
 * PURE. It takes a draw context and a rect and returns nothing; the IP is
 * passed IN rather than read here, so it tests headlessly against the harness
 * framebuffer and so the (syscall-walking) read can be cached by the caller
 * instead of happening on every frame.
 *
 * ⚠ THE QR IS CACHED BY URL, not per frame. Encoding a version-2 symbol is a
 * Reed-Solomon pass over 44 codewords plus eight full mask-and-score
 * evaluations of a 625-module grid — perfectly cheap once, absurd sixty times a
 * second, and this screen is redrawn every tick like every other. The cache is
 * one entry deep because the URL changes about as often as the DHCP lease.
 */

import { encode } from "./qr.mjs";

/** The port Schwung Manager listens on. Named once; nothing else may spell it. */
export const MANAGER_PORT = 7700;

/* The top of the band the hint footer owns (list_geometry's RULE_Y). Imported
 * as a number rather than from that module so this file stays free of the UI
 * layout graph and testable on its own; the two are pinned together by
 * tests/host/test_connect_screen.sh. */
export const FOOTER_TOP = 55;

/*
 * MODULE SCALE 2, AND THAT IS WHY THIS IS A FULL-SCREEN VIEW.
 *
 * The URL is 20-28 bytes, which is past version 1's 17-byte capacity at level
 * L, so the symbol is 25 modules square and no arrangement of the text changes
 * that. At one pixel per module it is 25px on a ~1.7" panel -- about a third of
 * a millimetre per module, which a phone camera can sometimes resolve and
 * cannot be relied on to. At two it is 50px, which is comfortable.
 *
 * With the quiet zone the plate is 58px, and the body between a header and the
 * footer is 46. That is why the Connect screen is a view of its own and not a
 * page inside Global Settings' chrome, and why it draws no header: there is
 * room for the symbol or for a title bar, not both.
 */
export const QR_SCALE = 2;

/*
 * ⚠ DARK MODULES ON A LIT PLATE -- the symbol is drawn in NEGATIVE against this
 * display, and that is the right way round.
 *
 * On an OLED an unlit pixel is black, so painting the modules lit gives white
 * modules on a black field: an INVERTED QR. Phones mostly cope (iOS Camera and
 * Google Lens both invert and retry) and a scanner that does not simply sees
 * nothing, with no way for the user to tell why. Painting a lit rectangle and
 * putting the modules in as unlit pixels gives the polarity every reader
 * expects, and the quiet zone comes with it for free.
 *
 * TWO MODULES OF QUIET ZONE, not the specification's four. Four makes the plate
 * 66px on a 64px-tall screen -- it does not fit at any scale that is worth
 * scanning, so the choice is between a smaller margin and a smaller symbol.
 * Measured with a real decoder (see tools/qr/verify_qr.py's sibling experiment,
 * recorded here because the numbers are the argument): at scale 2, quiet zones
 * of 3, 2 and 1 all decode and 0 does not. Two leaves a module of headroom over
 * the last value that works, which is the most a 64px screen can afford.
 */
export const QR_QUIET = 2;

/** The lit plate a symbol needs, in pixels, including its quiet zone. */
export function plateSize(symbol) {
    return symbol ? (symbol.size + 2 * QR_QUIET) * QR_SCALE : 0;
}

/** The URL a phone should open, or "" when the device has no usable address. */
export function managerUrl(ip) {
    const s = String(ip == null ? "" : ip).trim();
    return s ? `http://${s}:${MANAGER_PORT}` : "";
}

let cachedUrl = null;
let cachedSymbol = null;

/**
 * The QR for a URL, encoded at most once per URL.
 *
 * Returns null rather than throwing: an address long enough to overflow version
 * 4 cannot happen from an IPv4 literal, but a caller handed something strange
 * must get a screen with no QR on it, not a dead UI. The one-entry cache also
 * holds the failure, so a bad URL costs one attempt rather than one per frame.
 */
export function qrFor(url) {
    if (!url) return null;
    if (url === cachedUrl) return cachedSymbol;
    cachedUrl = url;
    try {
        cachedSymbol = encode(url);
    } catch (e) {
        cachedSymbol = null;
    }
    return cachedSymbol;
}

/**
 * Paint the symbol, merging horizontal runs into one fillRect each.
 *
 * A version-2 symbol is 625 modules and roughly half of them are dark, so the
 * naive one-rect-per-module draw is ~300 calls across the JS/C boundary every
 * frame. Runs cut that to ~90 with no change to a single pixel: a QR's dark
 * modules are strongly clustered horizontally, which is the same property that
 * makes the mask penalty rules score runs in the first place.
 */
export function drawQr(ctx, symbol, x, y, scale) {
    if (!symbol) return;
    const { size, modules } = symbol;
    /* The lit plate, quiet zone included. Drawn here rather than by the caller
     * so the margin cannot be forgotten -- a symbol with no quiet zone is the
     * one failure mode that looks completely fine on screen. */
    const plate = plateSize(symbol);
    ctx.fillRect(x, y, plate, plate, 1);
    const ox = x + QR_QUIET * scale;
    const oy = y + QR_QUIET * scale;
    for (let my = 0; my < size; my++) {
        const row = modules[my];
        let runStart = -1;
        for (let mx = 0; mx <= size; mx++) {
            const on = mx < size && row[mx];
            if (on && runStart < 0) runStart = mx;
            else if (!on && runStart >= 0) {
                ctx.fillRect(ox + runStart * scale, oy + my * scale,
                             (mx - runStart) * scale, scale, 0);
                runStart = -1;
            }
        }
    }
}

/*
 * The host address, split across as many lines as the column can hold.
 *
 * "255.255.255.255" is 81px in the 5x7 font against a 69px column, so the
 * longest address there is cannot be printed whole beside the symbol. Printed
 * anyway it is CLIPPED BY THE FRAMEBUFFER, and what survives is a shorter
 * address that still looks like one -- the worst failure this screen has, since
 * the whole reason it exists is that nobody knew the number.
 *
 * So it is split at a dot, which is the one place an address can break and
 * still read as itself, and the trailing dot is kept on the first line so the
 * two halves cannot be mistaken for two addresses. Splitting is attempted
 * ONCE: two lines cover every IPv4 literal at every column width this screen
 * uses, and a general recursive wrap would be untested code for a case that
 * cannot occur.
 *
 * @param {(t:string)=>boolean} fits  measured against the real font by the
 *   caller, never guessed from a character count -- the 5x7 atlas is
 *   proportional (a '1' is narrower than a '2') so "fifteen characters" is not
 *   a width.
 */
export function hostLines(host, fits) {
    const h = String(host || "");
    if (!h || fits(h)) return [h];
    const dots = [];
    for (let i = 0; i < h.length; i++) if (h[i] === ".") dots.push(i);
    if (!dots.length) return [h];
    /* The dot nearest the middle gives the two most even halves, which is what
     * keeps the second line from being the one that overflows instead. */
    const mid = h.length / 2;
    let at = dots[0];
    for (const d of dots) if (Math.abs(d - mid) < Math.abs(at - mid)) at = d;
    return [h.slice(0, at + 1), h.slice(at + 1)];
}

/**
 * Draw the whole screen into `rect`.
 *
 * ⚠ NO HEADER, AND THE CALLER MUST NOT DRAW ONE. See QR_SCALE: the plate is
 * 58px and the body between a header and the footer is 46. The right-hand
 * column says "Schwung Manager", which titles the screen better than a header
 * bar would have.
 *
 * The caller MAY draw the ordinary hint footer: the plate occupies the left
 * ~60px and the footer's pills are laid out from the right edge, so the two do
 * not meet. `tests/host/test_connect_screen.sh` asserts that rather than
 * trusting it, because it is a property of two files that do not know about
 * each other.
 *
 * @param {object} ctx  fillRect / print / textWidth
 * @param {object} rect {x, y, w, h} — the whole panel.
 * @param {object} o
 * @param {string} o.ip  the device's address, or "" when it has none.
 */
export function drawConnectBody(ctx, rect, { ip } = {}) {
    const url = managerUrl(ip);
    const measure = (t) => (typeof ctx.textWidth === "function"
        ? ctx.textWidth(String(t)) : String(t).length * 6);

    /*
     * NO ADDRESS IS A SENTENCE, NOT A BLANK PANEL.
     *
     * The one state this screen must not have is "looks broken": it is opened
     * precisely when something is not working, and an empty rect with no words
     * is indistinguishable from a crash. It equally must not print a
     * plausible-looking URL for an address it does not have, which is what any
     * default would do.
     *
     * move.local is offered here and ONLY here -- as the last thing left to try
     * on a device with no address to give, rather than as the primary
     * instruction every pointer screen used to print unconditionally.
     */
    if (!url) {
        /* Six lines, because seven runs the last one into the footer band and
         * the widest of them has to clear 128px -- both measured against the
         * real font by tests/host/test_connect_screen.sh rather than counted. */
        const lines = ["No network.", "",
                       "Join WiFi, then open", "this page again.", "",
                       "Or try move.local:7700"];
        let y = rect.y + Math.max(0, Math.floor((Math.min(rect.h, FOOTER_TOP) - lines.length * 8) / 2));
        for (const line of lines) {
            if (line) ctx.print(rect.x + 3, y, line, 1);
            y += 8;
        }
        return;
    }

    const symbol = qrFor(url);
    const plate = plateSize(symbol);
    const qrX = rect.x + 2;
    const qrY = rect.y + Math.max(0, Math.floor((rect.h - plate) / 2));
    drawQr(ctx, symbol, qrX, qrY, QR_SCALE);

    /* Derived from the plate rather than written down, so a version-1 symbol
     * (21 modules, if a shorter URL ever fits one) moves the column in instead
     * of leaving a gap the layout never hears about. */
    const tx = qrX + plate + 3;
    const colW = rect.x + rect.w - tx - 1;
    const fits = (t) => measure(t) <= colW;

    const lines = ["Schwung", "Manager", "", ...hostLines(ip, fits), ":" + MANAGER_PORT];
    /*
     * Centred as a BLOCK against the symbol beside it -- a two-line address and
     * a one-line address are different heights, and anchoring to the top leaves
     * the short case hanging above the symbol it labels.
     *
     * Centred in the FOOTER-LESS height, not in the panel: the caller draws its
     * hint pills across the bottom band, and a six-line block centred on 64
     * would put ":7700" underneath them.
     */
    const bodyH = Math.min(rect.h, FOOTER_TOP);
    const blockH = lines.length * 8;
    let ty = rect.y + Math.max(0, Math.floor((bodyH - blockH) / 2));
    for (const line of lines) {
        if (line) ctx.print(tx, ty, line, 1);
        ty += 8;
    }
}
