/*
 * Faderfox EC4 live SysEx: text on the display, and what the device reports.
 *
 * Faderfox publishes no spec for this. It is reconstructed from two
 * independent implementations that agree byte for byte:
 *   - Faderfox_Universal_2, Faderfox's own Ableton Live script (consts.py,
 *     faderfox_display_element.py, faderfox_parameter_display.py,
 *     FaderfoxSurface.py)
 *   - DrivenByMoss, the Bitwig extension (controller/faderfox/ec4/controller/
 *     EC4Display.java, EC4ControlSurface.java)
 * and from the EC4 manual V03, which documents that these exist ("Special
 * fixed commands (Sysex)") but not their bytes. All of it has since been
 * confirmed on hardware (firmware 2.00); docs/EC4_SURFACE.md has what was
 * measured.
 *
 * Wire format. Every message is
 *     F0 00 00 00  4E 2C 1B  <commands>  F7
 * and every command is three bytes, the same nibble encoding the setup dump
 * uses (schwungSetupMsg below): 0x4X command, 0x2X high nibble, 0x1X low
 * nibble. `4E 2C 1B` is itself one: the low nibble B is the EC4's device id
 * (0x0B), and FaderfoxSurface.py identifies the device from exactly that byte.
 *
 *   4E 22 1p       select text page p: 0 = the 16 encoder-name cells (64
 *                  chars), 3 = the 4x20 overlay ("total display", 80 chars).
 *                  Pages 1 and 2 are unused by both references; text
 *                  written to them on hardware shows nowhere.
 *   4A 2h 1l       set the write offset within the page
 *   4D 2h 1l       write one character at the offset, which then advances
 *   4E 22 14 / 15  show / hide the overlay
 *
 * Several offset+run pairs may share one message (DrivenByMoss sends its
 * diff that way). Encoder names only show host text where the stored name is
 * '----' (manual V03: "Set encoder names to '----' else the script can't
 * write the names").
 *
 * The one message WITHOUT the device id is the request:
 *     F0 00 00 00 4E 20 10 F7
 * answered with the current setup and group. The same commands arrive
 * unasked when the user changes setup or group, or presses Shift, a user key
 * (FUNC + encoder 1/5/9/13) or Shift + an encoder push:
 *     4E 28 1s        setup s (0-15)
 *     4E 24 1g        group g (0-15)
 *     4E 26 1k        extended key: 1 = Shift, 2-5 = user keys 1-4
 *     4E 2A 1n        shifted push button n (0-15)
 *     4E 2E 1v        state of the key just named: 1 = pressed, 0 = released
 *
 * Message sizes matter here more than anywhere: Move splices its own MIDI into
 * a SysEx that spans SPI frames (docs/E16_REMOTE.md, "The garbling"), and the
 * host places a message of <= 12 USB-MIDI packets whole within one frame. One
 * 4-character cell is 26 bytes / 9 packets and fits; all 64 name characters
 * in one message are 206 bytes / 69 packets and do not.
 */

export const HEADER = [0xF0, 0x00, 0x00, 0x00, 0x4E, 0x2C, 0x1B];
export const DEVICE_ID = 0x0B;

export const PAGE_NAMES = 0;
export const PAGE_TOTAL = 3;
export const NAMES_CHARS = 64;
export const TOTAL_ROWS = 4;
export const TOTAL_COLS = 20;
export const TOTAL_CHARS = TOTAL_ROWS * TOTAL_COLS;

const CMD_FUNC = 0x4E;
const CMD_OFFSET = 0x4A;
const CMD_DATA = 0x4D;

const FUNC_PAGE = 0x22;        /* value 0x10 | page, 0x14 show, 0x15 hide */
const FUNC_SETUP = 0x28;
const FUNC_GROUP = 0x24;
const FUNC_EXT_KEY = 0x26;
const FUNC_SHIFTED_KEY = 0x2A;
const FUNC_KEY_STATE = 0x2E;

const SHOW_TOTAL = 0x14;
const HIDE_TOTAL = 0x15;

export const EXT_KEY_SHIFT = 1;

function nib(v) {
    return [0x20 | ((v >> 4) & 0x0F), 0x10 | (v & 0x0F)];
}

/* The display's character ROM, from consts.py CHARS: 16 rows of 16, so a
 * character's index is its code. It is ASCII for space, digits, A-Z, a-z and
 * most punctuation; '$', '@' (at 0x40) and '[\]' are not where ASCII puts
 * them, and umlauts sit in the 0x5B/0x7B rows. Anything else becomes 0x1F,
 * which is what the Faderfox script sends for an unknown character. */
const ROM = [
    '                ',
    '                ',
    ' !"# %&\'()*+,-./',
    '0123456789:;<=>?',
    ' ABCDEFGHIJKLMNO',
    'PQRSTUVWXYZÄÖ Ü§',
    ' abcdefghijklmno',
    'pqrstuvwxyzäö üà',
    '  ²³            ',
    '          ()    ',
    '@               ',
    '                ',
    '    _           ',
    '                ',
    '                ',
    '          [\\]<|>',
].join('');
/* '(' and ')' appear twice (0x28/0x29 and 0x9A/0x9B). The Python dict in
 * consts.py keeps the LATER index; DrivenByMoss sends plain ASCII, i.e. the
 * earlier one. Earlier wins here: it is ASCII, and it is what the second
 * reference puts on the wire. */
const CODE = new Map();
for (let i = ROM.length - 1; i >= 0; i--) if (ROM[i] !== ' ') CODE.set(ROM[i], i);
CODE.set(' ', 0x20);
/* The full block, 0x1F: a whole bar cell. Photographed on firmware 2.00
 * (every code 0x00-0xFF written to the overlay); Faderfox's script maps no
 * character to it and uses the code only as its "unknown" glyph. */
export const BLOCK = '\u2588';
CODE.set(BLOCK, 0x1F);
export const UNKNOWN_CHAR = 0x1F;

export function charCode(ch) {
    const c = CODE.get(ch);
    return c === undefined ? UNKNOWN_CHAR : c;
}

/* runs: [{offset, text}] on one page. Offsets are character positions within
 * the page: cell n of the names page starts at n * 4, row r of the overlay at
 * r * 20. */
export function textMsg(page, runs, opts) {
    const out = HEADER.slice();
    out.push(CMD_FUNC, FUNC_PAGE, 0x10 | (page & 0x0F));
    for (const run of runs) {
        out.push(CMD_OFFSET, ...nib(run.offset));
        for (const ch of run.text) out.push(CMD_DATA, ...nib(charCode(ch)));
    }
    if (opts && opts.show) out.push(CMD_FUNC, FUNC_PAGE, SHOW_TOTAL);
    out.push(0xF7);
    return out;
}

export function showTotalMsg() {
    return HEADER.concat([CMD_FUNC, FUNC_PAGE, SHOW_TOTAL, 0xF7]);
}

export function hideTotalMsg() {
    return HEADER.concat([CMD_FUNC, FUNC_PAGE, HIDE_TOTAL, 0xF7]);
}

/* Current setup and group. No device id: FaderfoxSurface.py and
 * EC4ControlSurface.java both send exactly these eight bytes. */
export function queryMsg() {
    return [0xF0, 0x00, 0x00, 0x00, CMD_FUNC, 0x20, 0x10, 0xF7];
}

/* msg: a whole SysEx, F0..F7. Returns null if it is not from an EC4,
 * otherwise a list of events:
 *   {type: 'setup', setup}      0-15
 *   {type: 'group', group}      0-15
 *   {type: 'key', key, pressed} key: 'shift' | 'user1'..'user4' | 'push0'..'push15'
 *                               (push = Shift + encoder push)
 *   {type: 'unknown', cmd, func, value}  a well-formed command we do not know
 * The walk mirrors EC4ControlSurface.handleSysexCommandsController: a key
 * state names whichever key the same message named before it. */
export function parse(msg) {
    if (!msg || msg.length < HEADER.length + 1) return null;
    for (let i = 0; i < HEADER.length; i++) if (msg[i] !== HEADER[i]) return null;
    if (msg[msg.length - 1] !== 0xF7) return null;
    const body = msg.slice(HEADER.length, msg.length - 1);
    const events = [];
    let extKey = -1, shiftedKey = -1;
    for (let i = 0; i < body.length; i += 3) {
        const cmd = body[i], func = body[i + 1], value = body[i + 2];
        if (i + 3 > body.length) { events.push({ type: 'unknown', cmd, func, value }); break; }
        if (cmd !== CMD_FUNC || (value & 0xF0) !== 0x10) {
            events.push({ type: 'unknown', cmd, func, value });
            continue;
        }
        const v = value & 0x0F;
        if (func === FUNC_SETUP) events.push({ type: 'setup', setup: v });
        else if (func === FUNC_GROUP) events.push({ type: 'group', group: v });
        else if (func === FUNC_EXT_KEY) extKey = v;
        else if (func === FUNC_SHIFTED_KEY) shiftedKey = v;
        else if (func === FUNC_KEY_STATE) {
            const pressed = v === 1;
            if (shiftedKey >= 0) events.push({ type: 'key', key: 'push' + shiftedKey, pressed });
            else if (extKey === EXT_KEY_SHIFT) events.push({ type: 'key', key: 'shift', pressed });
            else if (extKey >= 2 && extKey <= 5) events.push({ type: 'key', key: 'user' + (extKey - 1), pressed });
            else events.push({ type: 'unknown', cmd, func, value });
        } else events.push({ type: 'unknown', cmd, func, value });
    }
    return events;
}

/* Wire bytes -> USB-MIDI packets for move_midi_external_send. Same framing as
 * the E16's; kept local so this module has no E16 dependency. */
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


/*
 * THE SCHWUNG SETUP, AS ONE SINGLE-SETUP DOWNLOAD.
 *
 * The EC4 has no remote mode: what an encoder sends is whatever its setup
 * says. So Schwung installs a setup whose every group sends the E16's
 * remote-mode map -- CC 1-16 relative (1/127, no acceleration), notes 0-15
 * momentary, on one channel -- which the E16 input decoder and the shim's
 * claim (src/host/e16_claim.h) already serve. Encoder names are "----", the
 * EC4's marker for a cell the host may write (manual V03).
 *
 * The format is Faderfox's setup dump (faderfox-editor, doc/faderfox sysex
 * format general.txt and ... data format EC4 V2.txt): every byte as three
 * wire bytes (4X command, 2X high nibble, 1X low nibble), memory in 64-byte
 * pages, each closed by a 16-bit sum of its data and 30 zero bytes.
 * DOWNLOAD TYPE 2 is one setup: the EC4's own "Send current setup" is this
 * message byte for byte (captured on firmware 2.00) -- 61 pages at the setup's
 * addresses: push buttons part 1 (0x0B00 + s*256, 4 pages), group names
 * (0x1C00 + s*64, 1), encoders (0x2000 + s*3072, 48), push buttons part 2
 * (0xE000 + s*512, 8). No setup name travels in it.
 *
 * MEASURED ON RECEIPT: the EC4 writes a single setup into its CURRENT setup
 * and ignores these addresses; every other setup is untouched. So the setup
 * to replace is chosen on the EC4 itself, before entering receive mode, and
 * `setup` here only fills in the addresses the device would have sent.
 *
 * Every byte of those four regions is written, so the message depends on
 * nothing already on the device.
 */
const SETUP_PAGES = (s) => [].concat(
    [0, 1, 2, 3].map((k) => 0x0B00 + s * 256 + k * 64),
    [0x1C00 + s * 64],
    Array.from({ length: 48 }, (_, k) => 0x2000 + s * 3072 + k * 64),
    Array.from({ length: 8 }, (_, k) => 0xE000 + s * 512 + k * 64));
const DUMP_DEVICE = 0x0B, DUMP_TYPE_ONE_SETUP = 0x02, DUMP_APP_H = 0x02, DUMP_APP_L = 0x00;
const DUMP_PADDING = 30;

export function schwungSetupMsg(setup, channel) {
    const s = setup & 0x0F, ch = (channel || 0) & 0x0F;
    const mem = new Map();
    const put = (addr, v) => mem.set(addr, v & 0xFF);
    const putName = (addr, text) => { for (let i = 0; i < 4; i++) put(addr + i, text.charCodeAt(i)); };
    for (let g = 0; g < 16; g++) {
        putName(0x1C00 + (s * 16 + g) * 4, 'G' + String(g + 1).padStart(2, '0') + ' ');
        const base = 0x2000 + (s * 16 + g) * 192;
        const k1 = 0x0B00 + (s * 16 + g) * 16, k2 = 0xE000 + (s * 16 + g) * 32;
        for (let e = 0; e < 16; e++) {
            put(base + e, (0 << 4) | ch);          /* type CCR1 (relative 1/127) + channel */
            put(base + 16 + e, 1 + e);             /* no link, CC 1-16 */
            put(base + 32 + e, 0);                 /* NRPN MSB, unused */
            put(base + 48 + e, 0);                 /* lower */
            put(base + 64 + e, 127);               /* upper */
            put(base + 80 + e, (3 << 4) | 0);      /* mode Acc0 (none), scale off */
            put(base + 96 + e, 0);                 /* lower/upper MSBs */
            put(base + 112 + e, (1 << 4) | ch);    /* push: note, channel */
            putName(base + 128 + e * 4, '----');   /* host-writable */
            put(k1 + e, e);                        /* key mode, note 0-15 */
            put(k2 + e, 0);                        /* no display, lower 0 */
            put(k2 + 16 + e, 127);                 /* no link, upper 127 */
        }
    }
    const out = [0xF0, 0x00, 0x00, 0x00];
    for (const [cmd, v] of [[0x41, DUMP_DEVICE], [0x42, DUMP_TYPE_ONE_SETUP], [0x43, DUMP_APP_H], [0x44, DUMP_APP_L]]) {
        out.push(cmd, ...nib(v));
    }
    for (const addr of SETUP_PAGES(s)) {
        out.push(0x49, ...nib(addr >> 8), 0x4A, ...nib(addr & 0xFF));
        let crc = 0;
        for (let i = 0; i < 64; i++) {
            const v = mem.has(addr + i) ? mem.get(addr + i) : 0;
            out.push(0x4D, ...nib(v));
            crc += v;
        }
        crc &= 0xFFFF;
        out.push(0x4B, ...nib(crc >> 8), 0x4C, ...nib(crc & 0xFF));
        for (let i = 0; i < DUMP_PADDING; i++) out.push(0x00);
    }
    out.push(0x4F, ...nib(DUMP_DEVICE), 0xF7);
    return out;
}
