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
     * bit-7 rule cannot apply here.  Every real SysEx packet still carries at
     * least one nonzero byte (F0, F7, or data), so an all-zero payload is a
     * stale slot rather than a message. */
    return (status || d1 || d2) ? 1 : 0;
}
