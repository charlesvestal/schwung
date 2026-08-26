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
    int changed = 0;

    for (int i = 0; i + 4 <= len; i += 4) {
        uint8_t cable = (midi_out[i] >> 4) & 0x0F;
        uint8_t cin = midi_out[i] & 0x0F;
        int n = cin_payload_len(cin);
        if (n == 0) continue;   /* non-SysEx slot: skip without resetting */
        if (cable != 0) continue;  /* only Move's own firmware speaks cable 0;
                                     * skip without disturbing reassembly. */

        const uint8_t *p = &midi_out[i + 1];
        if (p[0] == 0xF0) { st->rx_len = 0; st->rx_active = 1; }  /* data bytes are < 0x80 */
        if (!st->rx_active) continue;

        for (int k = 0; k < n; k++)
            if (st->rx_len < (int)sizeof st->rx_buf) st->rx_buf[st->rx_len++] = p[k];

        if (!cin_is_end(cin)) continue;
        st->rx_active = 0;
        if (!envelope_valid(st->rx_buf, st->rx_len)) continue;

        if (st->rx_buf[7] == XMOS_AUDIO_KEY_ROUTE) {
            memcpy(st->route, st->rx_buf, XMOS_AUDIO_MSG_LEN);
            st->have_route = 1;
            /* Track bit1 separately. It is not redundant with usbc_out: Move's
             * sampling page sends a LONE 37 12 to set bit0 and carries bit1
             * from its own stale "Mic" state, which clears monitoring and so
             * reverts the hardware to Mic with no 37 14 to show for it.
             * Deliberately NOT folded into `changed` — that flag means "the
             * out-source selection moved", and this is not a selection. */
            st->monitor = (st->rx_buf[8] & XMOS_AUDIO_ROUTE_BIT_MONITOR) ? 1 : 0;
        } else if (st->rx_buf[7] == XMOS_AUDIO_KEY_OUT_SRC) {
            int8_t out = (st->rx_buf[8] & 0x01) ? 1 : 0;
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
    out_mon[7] = XMOS_AUDIO_KEY_OUT_SRC;
    out_mon[8] = usbc_out ? 1 : 0;
    out_mon[XMOS_AUDIO_MSG_LEN - 1] = 0xF7;
}

int xmos_audio_emit(uint8_t *midi_out, int len, const uint8_t *msg) {
    /* Refuse if a cable-0 SysEx is mid-flight (started but not yet
     * terminated) anywhere in the buffer — its continuation is presumably
     * arriving next frame, and splicing our own F0 in among the free slots
     * would corrupt both messages. A complete, already-terminated cable-0
     * SysEx sitting in the buffer — including a message we just emitted
     * ourselves — does not block a subsequent emit. Caller already retries
     * next frame, so refusing here is always safe. */
    {
        int active = 0;
        for (int i = 0; i + 4 <= len; i += 4) {
            uint8_t cable = (midi_out[i] >> 4) & 0x0F;
            uint8_t cin = midi_out[i] & 0x0F;
            if (cable != 0 || cin < 0x04 || cin > 0x07) continue;
            if (midi_out[i + 1] == 0xF0) active = 1;
            if (cin == 0x05 || cin == 0x06 || cin == 0x07) active = 0;
        }
        if (active) return 0;
    }

    /* Find a contiguous run of XMOS_AUDIO_PACKETS free slots. Free-but-
     * scattered slots are not enough: a message must land as one unbroken
     * run so nothing else can interleave into it before Move parses it. */
    int run_start = -1, run_len = 0;
    for (int i = 0; i + 4 <= len; i += 4) {
        int free_slot = !midi_out[i] && !midi_out[i+1] && !midi_out[i+2] && !midi_out[i+3];
        if (free_slot) {
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len >= XMOS_AUDIO_PACKETS) break;
        } else {
            run_len = 0;
        }
    }
    if (run_len < XMOS_AUDIO_PACKETS) return 0;

    /* Whole message or nothing: the run found above is guaranteed to hold
     * exactly XMOS_AUDIO_PACKETS packets, so this always completes without
     * ever leaving a half-written SysEx in MIDI_OUT. */
    int pos = 0, slot = run_start;
    while (pos < XMOS_AUDIO_MSG_LEN) {
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
