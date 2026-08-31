#include <string.h>

#include "shadow_midi_filter.h"

int shadow_midi_forwardable(uint8_t head, uint8_t status, uint8_t d1, uint8_t d2)
{
    if (head == 0) return 0;

    uint8_t cin = head & 0x0F;
    if (cin < 0x04 || cin > 0x0F) return 0;

    if (cin >= 0x08) {
        /* Channel voice / system: status byte always has bit 7 set. */
        return (status & 0x80) ? 1 : 0;
    }

    /* SysEx (CIN 0x04-0x07): payload bytes are legitimately < 0x80, so the
     * bit-7 rule cannot apply here.
     *
     * The previous rule was "at least one nonzero byte", on the reasoning that
     * a real SysEx packet always carries an F0, an F7, or data.  THAT IS FALSE
     * FOR CIN 0x04, and the exception is not rare: 0x04 is a three-byte
     * continuation whose payload is arbitrary 7-bit data, so `00 00 00` is a
     * zeroed parameter run — which is most of a patch dump.  Dropping it
     * corrupts the message SILENTLY: framing still holds because F0 and F7
     * both arrive in other packets, and the device's own checksum still
     * validates, because Roland and Yamaha checksums are sums mod 128 and the
     * removed bytes sum to zero.  The receiver gets a short, well-formed,
     * checksum-valid, WRONG message.
     *
     * Observed on a Roland JV-880 patch dump: every area reply short by an
     * exact multiple of 3, deterministic per patch, varying with patch
     * CONTENT.  The 9-byte SysEx prefix is a multiple of 3, so data offset 0
     * falls on a packet boundary — across 15 captured messages not one
     * ALIGNED all-zero triplet survived, while 47 UNALIGNED ones did.
     *
     * The stale-slot protection is kept, and for three of the four CINs it is
     * now STRONGER than the nonzero test it replaces.  USB-MIDI fixes the
     * length of an end-packet, so its final byte MUST be F7:
     *
     *   0x04  three data bytes, no constraint  -> anything, including 00 00 00
     *   0x05  ends with one byte               -> a status byte (F7, or a
     *                                             single-byte system message)
     *   0x06  ends with two bytes              -> d1 == F7
     *   0x07  ends with three bytes            -> d2 == F7
     *
     * Only 0x04 loses protection, and only against a stale slot whose CIN
     * nibble lands on exactly 0x04 with a zeroed payload. */
    switch (cin) {
    case 0x04: return 1;
    case 0x05: return (status & 0x80) ? 1 : 0;
    case 0x06: return (d1 == 0xF7) ? 1 : 0;
    case 0x07: return (d2 == 0xF7) ? 1 : 0;
    default:   return 0;
    }
}

int shadow_midi_in_slot_empty(const uint8_t *slot)
{
    if (!slot) return 1;
    return (slot[0] == 0 && slot[1] == 0 && slot[2] == 0 && slot[3] == 0) ? 1 : 0;
}

int shadow_midi_in_compact(uint8_t *midi_in)
{
    if (!midi_in) return 0;

    int w = 0;
    for (int r = 0; r < SHADOW_MIDI_IN_BYTES; r += SHADOW_MIDI_IN_STRIDE) {
        if (shadow_midi_in_slot_empty(&midi_in[r])) continue;
        if (w != r) memcpy(&midi_in[w], &midi_in[r], SHADOW_MIDI_IN_STRIDE);
        w += SHADOW_MIDI_IN_STRIDE;
    }
    if (w < SHADOW_MIDI_IN_BYTES)
        memset(&midi_in[w], 0, (size_t)(SHADOW_MIDI_IN_BYTES - w));

    return w / SHADOW_MIDI_IN_STRIDE;
}
