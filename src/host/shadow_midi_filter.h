/* shadow_midi_filter.h — pure predicates for validating USB-MIDI packets
 * scanned out of the unfiltered hardware MIDI_IN buffer.
 *
 * No I/O, no allocation, no locks: safe to call from the SPI callback and
 * unit-testable on the dev host (see tests/host/test_shadow_midi_filter.c).
 */

#ifndef SHADOW_MIDI_FILTER_H
#define SHADOW_MIDI_FILTER_H

#include <stdint.h>

/* Should a USB-MIDI packet be forwarded to the shadow UI ring?
 *
 * `head` is the USB-MIDI header byte (cable << 4 | CIN); status/d1/d2 are the
 * three payload bytes.  Returns 1 to forward, 0 to drop.
 *
 * The hardware MIDI_IN buffer is never cleared wholesale, so consumed slots
 * keep their stale bytes and are re-scanned every SPI frame.  A slot is only
 * trustworthy when its payload is self-consistent with its CIN:
 *
 *   - CIN 0x08-0x0F (channel voice / system): the status byte always has bit 7
 *     set.  A sub-0x80 status is a torn or stale read.
 *   - CIN 0x04-0x07 (SysEx): payload bytes are legitimately < 0x80, so the
 *     bit-7 rule cannot apply.  But a SysEx packet always carries at least one
 *     nonzero byte (F0, F7, or data), so an all-zero payload is stale.
 *   - Any other CIN is not a packet we forward.
 */
int shadow_midi_forwardable(uint8_t head, uint8_t status, uint8_t d1, uint8_t d2);

#endif /* SHADOW_MIDI_FILTER_H */
