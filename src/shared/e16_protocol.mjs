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
 * Wire order is encoder, R, G, amount MSB, amount LSB, B, bipolar -- amount
 * sits between G and B, not after B. Pinned by the "ring amount split"
 * assertion in the test; get this wrong and the ring still draws (wrong
 * fields land in range for a while) so it will not fail loudly on hardware. */
export function ringMsg(rings) {
    const raw = [];
    for (const x of rings) {
        raw.push(x.enc & 0x0F, x.r & 0x7F, x.g & 0x7F,
                 (x.amount >> 7) & 0x7F, x.amount & 0x7F,
                 x.b & 0x7F, x.bipolar ? 1 : 0);
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
