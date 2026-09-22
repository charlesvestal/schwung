/*
 * OXI E16 remote-mode wire format. See docs/E16_REMOTE.md.
 *
 * Every builder returns RAW MESSAGE BYTES including F0/F7; packetize() turns
 * those into the flat 4-byte USB-MIDI packets move_midi_external_send wants.
 * The two are separate because the packetizing is Schwung's transport concern
 * and the message layout is OXI's -- and because a test that pins bytes is
 * only readable if the bytes are the message.
 */

const HDR = [0x00, 0x21, 0x5B, 0x02, 0x01];

/* One MSB byte per group of <=7, then those bytes with bit 7 cleared.
 *
 * For LABELS and RING every payload byte is already < 0x80, so the MSB byte is
 * always zero and the packing looks like padding. It is not: FRAMEBUFFER
 * carries real pixel bytes with bit 7 set, and a packer written to emit a
 * constant zero passes every test but the one that matters. */
export function pack7(raw) {
    const out = [];
    for (let i = 0; i < raw.length; i += 7) {
        const n = Math.min(7, raw.length - i);
        let msbs = 0;
        for (let k = 0; k < n; k++) if (raw[i + k] & 0x80) msbs |= (1 << k);
        out.push(msbs);
        for (let k = 0; k < n; k++) out.push(raw[i + k] & 0x7F);
    }
    return out;
}

function msg(id, raw) {
    return [0xF0].concat(HDR, id, raw && raw.length ? pack7(raw) : [], [0xF7]);
}

export function enterMsg() { return msg([0x06, 0x55]); }
export function exitMsg()  { return msg([0x06, 0x00]); }

export const ACK_BODY = HDR.concat([0x06, 0x53]);

/* rings: [{enc, r, g, b, amount (0-16383), bipolar}] -- variable length, so a
 * single changed encoder costs one chunk rather than a whole-screen repaint.
 * That asymmetry is the feature's rate strategy, not an optimisation.
 *
 * Chunk order is encoder, R, G, B, amount MSB, amount LSB, bipolar. An earlier
 * revision put amount between G and B to satisfy a test assertion that was
 * itself off by one: index 8 is the pack7 group byte, so the payload starts at
 * 9 and the amount lands at 13/14, not 12/13. Ground truth is the Max patch on
 * the lines thread, which sends `0 3 0 12 0 $1 0 0` -- pack byte, encoder 3,
 * R 0, G 12, B 0, then the amount. Getting this wrong is invisible in a unit
 * test and shows up on hardware as rings that light the wrong colour at the
 * wrong position. */
export function ringMsg(rings) {
    const raw = [];
    for (const x of rings) {
        raw.push(x.enc & 0x0F, x.r & 0x7F, x.g & 0x7F, x.b & 0x7F,
                 (x.amount >> 7) & 0x7F, x.amount & 0x7F, x.bipolar ? 1 : 0);
    }
    return msg([0x06, 0x04], raw);
}

/* leds: [{enc, led, r, g, b}] */
export function ledMsg(leds) {
    const raw = [];
    for (const x of leds) raw.push(x.enc & 0x0F, x.led & 0x0F,
                                   x.r & 0x7F, x.g & 0x7F, x.b & 0x7F);
    return msg([0x06, 0x01], raw);
}

/* buf: 1024 bytes, SSD1306 page/column. Whole screen only -- the spec has no
 * partial update, which is why navigation is the expensive send. */
export function framebufferMsg(buf) {
    if (buf.length !== 1024) throw new Error("framebuffer must be 1024 bytes");
    return msg([0x06, 0x02], Array.from(buf));
}

/* title: 16 chars, labels: 16 strings of <=4. Kept for the fallback renderer;
 * the surface draws pixels instead. */
export function labelsMsg(title, labels) {
    const pad = (s, n) => (s + " ".repeat(n)).slice(0, n);
    const raw = [];
    for (const c of pad(title, 16)) raw.push(c.charCodeAt(0) & 0x7F);
    for (let i = 0; i < 16; i++)
        for (const c of pad(labels[i] || "", 4)) raw.push(c.charCodeAt(0) & 0x7F);
    return msg([0x06, 0x03], raw);
}

/* Flat 4-byte USB-MIDI packets. Cable nibble left at 0 -- js_shadow_midi_send
 * overwrites it with 2 for the external port. */
export function packetize(bytes) {
    const out = [];
    let i = 0;
    while (bytes.length - i > 3) {
        out.push(0x04, bytes[i], bytes[i + 1], bytes[i + 2]);
        i += 3;
    }
    const left = bytes.length - i;
    const cin = { 1: 0x05, 2: 0x06, 3: 0x07 }[left];
    out.push(cin, bytes[i] || 0, bytes[i + 1] || 0, bytes[i + 2] || 0);
    return out;
}

/* asm holds everything between F0 and F7. */
export function isAck(asm) {
    if (asm.length !== ACK_BODY.length) return false;
    for (let i = 0; i < ACK_BODY.length; i++) if (asm[i] !== ACK_BODY[i]) return false;
    return true;
}

/*
 * ---------------------------------------------------------------------------
 * PARTIAL OLED UPDATES -- built against OXI's DRAFT spec (shared 2026-09-21,
 * not yet shipped in firmware; see docs/superpowers/specs/2026-09-22-e16-
 * partial-oled-updates-design.md). Nothing here has been verified on
 * hardware.
 *
 * SPEC AMBIGUITY: the sheet's ID column lists every opcode as TWO bytes
 * (0x06 0xXX), and the 8 existing messages' example bytes agree. The 5 NEW
 * messages below (SCANLINE/RECTANGLE/CLEAR/ACK/NACK) list the same two-byte
 * ID, but their EXAMPLE bytes drop the 0x06 (e.g. SCANLINE's worked example
 * is "...02 01 05 [payload] F7", not "...01 06 05..."). A loose note at the
 * top of the sheet -- "0x06 is the category message, no longer necessary
 * once in REMOTE MODE" -- could explain this but doesn't clearly say it
 * applies only to these five. We follow the literal example bytes (the more
 * concrete evidence) behind this one flag so a wrong guess is a one-line fix
 * once real firmware answers.
 * ---------------------------------------------------------------------------
 */
export const OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX = false;

const OLED_SCANLINE_ID = 0x05;
const OLED_RECTANGLE_ID = 0x06;
const OLED_CLEAR_ID = 0x07;
const OLED_UPDATE_ACK_ID = 0x53;
const OLED_UPDATE_NACK_ID = 0x54;

function oledId(subId) {
    return OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX ? [0x06, subId] : [subId];
}

/* rowBits: 16 bytes, left to right, MSB first; 1 = on. No CRC -- see the
 * design doc; the field is optional in the spec and its algorithm is
 * undocumented. */
export function scanlineMsg(y, rowBits) {
    if (rowBits.length !== 16) throw new Error("scanline row must be 16 bytes");
    return msg(oledId(OLED_SCANLINE_ID), [y & 0x3F].concat(Array.from(rowBits)));
}

/* bits: ceil(w/8) * h bytes, row-major, MSB first, each row byte-aligned.
 * No CRC, same reason as scanlineMsg. */
export function rectangleMsg(x, y, w, h, bits) {
    const want = Math.ceil(w / 8) * h;
    if (bits.length !== want) {
        throw new Error("rectangle payload must be " + want + " bytes, got " + bits.length);
    }
    return msg(oledId(OLED_RECTANGLE_ID),
               [x & 0x7F, y & 0x3F, w & 0x7F, h & 0x3F].concat(Array.from(bits)));
}

export function clearMsg() {
    return msg(oledId(OLED_CLEAR_ID), []);
}

/* Inverse of pack7: packed groups of <=8 bytes (1 MSB byte + up to 7 payload
 * bytes) back to rawLen raw bytes. rawLen is required -- the packed stream
 * carries no length of its own, and pack7's last group can be short. */
export function unpack7(packed, rawLen) {
    const out = [];
    let pi = 0;
    for (let done = 0; done < rawLen; ) {
        const n = Math.min(7, rawLen - done);
        const msbs = packed[pi++];
        for (let k = 0; k < n; k++) {
            const lo = packed[pi++] & 0x7F;
            out.push(lo | (((msbs >> k) & 1) ? 0x80 : 0));
        }
        done += n;
    }
    return out;
}

/* asm holds everything between F0 and F7, as createSysexAssembler delivers
 * it. Returns { id, payload } if asm starts with our header and carries an
 * id of the expected width, else null -- payload is whatever pack7'd bytes
 * follow, unparsed. */
function oledReplyHeader(asm) {
    const idLen = OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX ? 2 : 1;
    if (asm.length < HDR.length + idLen) return null;
    for (let i = 0; i < HDR.length; i++) if (asm[i] !== HDR[i]) return null;
    if (idLen === 2) {
        if (asm[HDR.length] !== 0x06) return null;
        return { id: asm[HDR.length + 1], payload: asm.slice(HDR.length + 2) };
    }
    return { id: asm[HDR.length], payload: asm.slice(HDR.length + 1) };
}

/* cmd is the ORIGINAL COMMAND byte the device echoes back (its own SCANLINE/
 * RECTANGLE/CLEAR id, not ours -- the spec's ACK/NACK payload names which
 * update it is about). addr's four raw bytes decode per that command; 0xFF
 * in a field means "not applicable to this command", decoded to null rather
 * than the literal 255, per the tri-state read rule (CLAUDE.md: a filler
 * value must never be read as data). */
function decodeOledAddr(cmd, a) {
    const opt = (v) => (v === 0xFF ? null : v);
    if (cmd === OLED_SCANLINE_ID) return { y: opt(a[0]) };
    if (cmd === OLED_RECTANGLE_ID) {
        return { x: opt(a[0]), y: opt(a[1]), w: opt(a[2]), h: opt(a[3]) };
    }
    /* {} for CLEAR (genuinely no address fields) and for an unrecognised cmd
     * (firmware answering with a command id this file doesn't know) are
     * deliberately the same shape. A future cmd would need a new branch
     * here regardless -- there's no address layout to decode without one --
     * so there is nothing a caller could do differently for one case that
     * it couldn't already do by checking `cmd` itself. */
    return {};
}

/* Returns { ok, cmd, status, addr } for an OLED UPDATE ACK or NACK body, or
 * null for anything else -- including the unrelated REMOTE MODE ENTERED ACK,
 * which shares status byte 0x53 but never this length (it carries no
 * payload), AND a truncated/garbled reply whose payload doesn't unpack7 to
 * exactly 6 bytes. That last case collapses "not an OLED reply" and "an OLED
 * reply that arrived corrupted" into the same null -- deliberately, for now:
 * this feature has no retry/timeout state (see the design doc), so a caller
 * only ever asks "did this land or not", and both non-answers mean "no".
 * Revisit if a future caller needs to tell them apart. Check isAck() first
 * in a caller that cares about both ACK shapes, since that's the hot path. */
export function parseOledUpdateReply(asm) {
    const h = oledReplyHeader(asm);
    if (!h) return null;
    if (h.id !== OLED_UPDATE_ACK_ID && h.id !== OLED_UPDATE_NACK_ID) return null;
    const raw = unpack7(h.payload, 6);
    if (raw.length !== 6) return null;
    const [cmd, status, a0, a1, a2, a3] = raw;
    return { ok: h.id === OLED_UPDATE_ACK_ID, cmd, status, addr: decodeOledAddr(cmd, [a0, a1, a2, a3]) };
}
