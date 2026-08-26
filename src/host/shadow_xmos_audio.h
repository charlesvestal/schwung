/* shadow_xmos_audio.h - Move USB-C audio-out source (Mic / Main Out)
 *
 * Pure buffer helpers for the XMOS audio-IO SysEx envelope. No I/O, no
 * allocation, no locks — safe to call from the SPI callback.
 */
#ifndef SHADOW_XMOS_AUDIO_H
#define SHADOW_XMOS_AUDIO_H

#include <stdint.h>

/* F0 00 21 1D 01 01 37 <inner[15]> F7 */
#define XMOS_AUDIO_MSG_LEN 23
/* USB-MIDI SysEx framing packs 3 payload bytes per packet, with a final
 * shorter packet (1-3 bytes) carrying the terminator. */
#define XMOS_AUDIO_PACKETS ((XMOS_AUDIO_MSG_LEN + 2) / 3)
_Static_assert(XMOS_AUDIO_PACKETS == 8,
               "XMOS_AUDIO_PACKETS must match the 23-byte envelope framing");

#define XMOS_AUDIO_KEY_ROUTE   0x12  /* routing + monitoring TLV */
#define XMOS_AUDIO_KEY_OUT_SRC 0x14  /* dedicated out-source bit */

/* Bit0: selects the USB-C *input* Move records from. Owned by Move's sampling
 * page — we never write it. It survives untouched because xmos_audio_build()
 * reuses the last-observed route[] payload wholesale; this macro exists only
 * as documentation of the wire format, not live code. */
#define XMOS_AUDIO_ROUTE_BIT_USBC_IN 0x01
/* Bit1: engage monitoring. This is *how* Move routes Main Out to USB-C (the
 * XMOS mutes the speakers while it's set), which is why we drive it as the
 * out-source flag. */
#define XMOS_AUDIO_ROUTE_BIT_MONITOR 0x02

typedef struct {
    uint8_t  route[XMOS_AUDIO_MSG_LEN]; /* last observed 37 12 envelope */
    uint8_t  have_route;                /* 1 once route[] is populated */
    int8_t   usbc_out;                  /* -1 unknown, 0 = Mic, 1 = Main Out */
    /* Bit1 of the last observed 37 12, tracked separately from usbc_out
     * because Move's sampling page emits a LONE 37 12 to set bit0 (the USB-C
     * input select) and carries bit1 from its own permanently-stale "Mic" UI
     * state. Observed on hardware 2026-08-26: `37 12 01` then `37 12 00`, with
     * no 37 14 in the frame or anywhere near it. That clears monitoring — and
     * monitoring is *how* Main Out reaches USB-C — so the hardware reverts to
     * Mic while usbc_out still reads 1 and nothing re-asserts. -1 = unknown. */
    int8_t   monitor;
    uint32_t seq;                       /* bumped on every usbc_out change */

    /* Reassembly state. Persisted here (not on the call stack) because a
     * message can split across an SPI frame boundary — our own hardware
     * capture shows the pair occupying 16 of 20 MIDI_OUT slots, so any
     * concurrent LED/CC traffic forces Move to split it across two frames. */
    uint8_t  rx_buf[XMOS_AUDIO_MSG_LEN + 8];
    int      rx_len;
    int      rx_active;
} xmos_audio_state_t;

/* Zero-initializer with usbc_out set to -1 (unknown). A plain memset-zero
 * state reads usbc_out == 0, which is indistinguishable from an observed Mic
 * selection and would suppress the first real change. Always init state with
 * this macro rather than {0} or memset. */
#define XMOS_AUDIO_STATE_INIT { .usbc_out = -1, .monitor = -1 }

/* Scan a MIDI_OUT region (len bytes, 4-byte USB-MIDI slots) and fold any
 * audio-IO envelopes into st. Only cable-0 packets (Move's own firmware)
 * participate; other cables are skipped without disturbing in-progress
 * reassembly. Returns 1 if usbc_out changed. */
int xmos_audio_scan(const uint8_t *midi_out, int len, xmos_audio_state_t *st);

/* Build the replay pair for usbc_out (0 = Mic, 1 = Main Out). The route
 * message reuses the last payload Move sent so bit0 stays Move's. */
void xmos_audio_build(const xmos_audio_state_t *st, int usbc_out,
                      uint8_t out_route[XMOS_AUDIO_MSG_LEN],
                      uint8_t out_mon[XMOS_AUDIO_MSG_LEN]);

/* Write one message atomically into a contiguous run of free cable-0 slots.
 * Returns 1 on success, 0 if no contiguous run of XMOS_AUDIO_PACKETS free
 * slots exists, or if the buffer contains any unterminated cable-0 SysEx
 * (caller retries next frame). Never overwrites an occupied slot, and never
 * performs a partial write on failure. */
int xmos_audio_emit(uint8_t *midi_out, int len, const uint8_t *msg);

#endif /* SHADOW_XMOS_AUDIO_H */
