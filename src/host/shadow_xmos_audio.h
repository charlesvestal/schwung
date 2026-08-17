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
/* 23 bytes fragment as 7x3-byte packets + one 2-byte end packet. */
#define XMOS_AUDIO_PACKETS 8

#define XMOS_AUDIO_KEY_ROUTE 0x12  /* routing + monitoring TLV */
#define XMOS_AUDIO_KEY_MON   0x14  /* dedicated out-source bit */

#define XMOS_AUDIO_ROUTE_BIT_USBC_IN 0x01
#define XMOS_AUDIO_ROUTE_BIT_MONITOR 0x02

typedef struct {
    uint8_t  route[XMOS_AUDIO_MSG_LEN]; /* last observed 37 12 envelope */
    uint8_t  have_route;                /* 1 once route[] is populated */
    int8_t   usbc_out;                  /* -1 unknown, 0 = Mic, 1 = Main Out */
    uint32_t seq;                       /* bumped on every usbc_out change */
} xmos_audio_state_t;

/* Scan a MIDI_OUT region (len bytes, 4-byte USB-MIDI slots) and fold any
 * audio-IO envelopes into st. Returns 1 if usbc_out changed. */
int xmos_audio_scan(const uint8_t *midi_out, int len, xmos_audio_state_t *st);

/* Build the replay pair for usbc_out (0 = Mic, 1 = Main Out). The route
 * message reuses the last payload Move sent so bit0 stays Move's. */
void xmos_audio_build(const xmos_audio_state_t *st, int usbc_out,
                      uint8_t out_route[XMOS_AUDIO_MSG_LEN],
                      uint8_t out_mon[XMOS_AUDIO_MSG_LEN]);

/* Write one message into free slots. Returns 1 on success, 0 if fewer than
 * XMOS_AUDIO_PACKETS slots are free (caller retries next frame). Never
 * overwrites an occupied slot. */
int xmos_audio_emit(uint8_t *midi_out, int len, const uint8_t *msg);

#endif /* SHADOW_XMOS_AUDIO_H */
