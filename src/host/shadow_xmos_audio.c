/* shadow_xmos_audio.c - Move USB-C audio-out source (Mic / Main Out)
 *
 * Move's Settings menu exposes a USB-C audio-out source, but its firmware does
 * not persist it: every boot it reverts to Mic. Picking a value emits a pair of
 * XMOS audio-IO SysEx messages on MIDI_OUT within one SPI frame (captured on
 * hardware 2026-08-18):
 *
 *   Main Out:  F0 00 21 1D 01 01 37 12 02 00x12 F7
 *              F0 00 21 1D 01 01 37 14 01 00x12 F7
 *   Mic:       ...37 12 00...            ...37 14 00...
 *
 * `37 12` is the shared routing/monitoring TLV — bit0 selects the USB-C *input*
 * (owned by Move's sampling page, never ours to change), bit1 is monitoring.
 * `37 14` is the dedicated out-source bit. We observe both, persist the
 * preference, and re-assert it after boot.
 *
 * Everything here is pure buffer work — no I/O, no allocation, no locks — so
 * the SPI callback can call it and the host suite can compile it directly.
 */
#include <string.h>
#include "shadow_xmos_audio.h"

static const uint8_t XMOS_AUDIO_HDR[7] = { 0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x37 };

/* USB-MIDI CIN -> SysEx payload byte count. 0 = not a SysEx packet. */
static int cin_payload_len(uint8_t cin) {
    switch (cin & 0x0F) {
    case 0x4: return 3;  /* start / continue */
    case 0x5: return 1;  /* end, 1 byte */
    case 0x6: return 2;  /* end, 2 bytes */
    case 0x7: return 3;  /* end, 3 bytes */
    default:  return 0;
    }
}

static int cin_is_end(uint8_t cin) {
    uint8_t c = cin & 0x0F;
    return c == 0x5 || c == 0x6 || c == 0x7;
}

static int envelope_valid(const uint8_t *buf, int len) {
    if (len != XMOS_AUDIO_MSG_LEN) return 0;
    if (memcmp(buf, XMOS_AUDIO_HDR, sizeof XMOS_AUDIO_HDR) != 0) return 0;
    return buf[XMOS_AUDIO_MSG_LEN - 1] == 0xF7;
}

int xmos_audio_scan(const uint8_t *midi_out, int len, xmos_audio_state_t *st) {
    /* Slightly oversized so an over-long SysEx overflows past 23 and is
     * rejected by the length check rather than aliasing onto a valid one. */
    uint8_t buf[XMOS_AUDIO_MSG_LEN + 8];
    int blen = 0;
    int active = 0;
    int changed = 0;

    for (int i = 0; i + 4 <= len; i += 4) {
        int n = cin_payload_len(midi_out[i]);
        if (n == 0) continue;  /* non-SysEx slot: skip without resetting */

        const uint8_t *p = &midi_out[i + 1];
        if (p[0] == 0xF0) { blen = 0; active = 1; }  /* data bytes are < 0x80 */
        if (!active) continue;

        for (int k = 0; k < n; k++)
            if (blen < (int)sizeof buf) buf[blen++] = p[k];

        if (!cin_is_end(midi_out[i])) continue;
        active = 0;
        if (!envelope_valid(buf, blen)) continue;

        if (buf[7] == XMOS_AUDIO_KEY_ROUTE) {
            memcpy(st->route, buf, XMOS_AUDIO_MSG_LEN);
            st->have_route = 1;
        } else if (buf[7] == XMOS_AUDIO_KEY_MON) {
            int8_t out = (buf[8] & 0x01) ? 1 : 0;
            if (st->usbc_out != out) {
                st->usbc_out = out;
                st->seq++;
                changed = 1;
            }
        }
    }
    return changed;
}

void xmos_audio_build(const xmos_audio_state_t *st, int usbc_out,
                      uint8_t out_route[XMOS_AUDIO_MSG_LEN],
                      uint8_t out_mon[XMOS_AUDIO_MSG_LEN]) {
    /* Reuse the route payload Move itself sent this boot so bit0 (USB-C input
     * select) stays whatever Move wants; flip only the monitoring bit. Fall
     * back to a bare envelope if Move hasn't spoken yet. */
    if (st->have_route) {
        memcpy(out_route, st->route, XMOS_AUDIO_MSG_LEN);
    } else {
        memset(out_route, 0, XMOS_AUDIO_MSG_LEN);
        memcpy(out_route, XMOS_AUDIO_HDR, sizeof XMOS_AUDIO_HDR);
        out_route[7] = XMOS_AUDIO_KEY_ROUTE;
        out_route[XMOS_AUDIO_MSG_LEN - 1] = 0xF7;
    }
    if (usbc_out) out_route[8] |= XMOS_AUDIO_ROUTE_BIT_MONITOR;
    else          out_route[8] &= (uint8_t)~XMOS_AUDIO_ROUTE_BIT_MONITOR;

    memset(out_mon, 0, XMOS_AUDIO_MSG_LEN);
    memcpy(out_mon, XMOS_AUDIO_HDR, sizeof XMOS_AUDIO_HDR);
    out_mon[7] = XMOS_AUDIO_KEY_MON;
    out_mon[8] = usbc_out ? 1 : 0;
    out_mon[XMOS_AUDIO_MSG_LEN - 1] = 0xF7;
}

int xmos_audio_emit(uint8_t *midi_out, int len, const uint8_t *msg) {
    int free_slots = 0;
    for (int i = 0; i + 4 <= len; i += 4)
        if (!midi_out[i] && !midi_out[i+1] && !midi_out[i+2] && !midi_out[i+3])
            free_slots++;
    if (free_slots < XMOS_AUDIO_PACKETS) return 0;

    int pos = 0, slot = 0;
    while (pos < XMOS_AUDIO_MSG_LEN) {
        while (slot + 4 <= len &&
               (midi_out[slot] || midi_out[slot+1] || midi_out[slot+2] || midi_out[slot+3]))
            slot += 4;
        if (slot + 4 > len) return 0;  /* unreachable after the count above */

        int remaining = XMOS_AUDIO_MSG_LEN - pos;
        uint8_t cin;
        int n;
        if (remaining > 3)       { cin = 0x04; n = 3; }
        else if (remaining == 3) { cin = 0x07; n = 3; }
        else if (remaining == 2) { cin = 0x06; n = 2; }
        else                     { cin = 0x05; n = 1; }

        midi_out[slot]     = cin;  /* cable 0, matching Move's own framing */
        midi_out[slot + 1] = msg[pos];
        midi_out[slot + 2] = n > 1 ? msg[pos + 1] : 0;
        midi_out[slot + 3] = n > 2 ? msg[pos + 2] : 0;
        pos += n;
        slot += 4;
    }
    return 1;
}
