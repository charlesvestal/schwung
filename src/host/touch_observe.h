/*
 * Which MIDI_IN slots are a capacitive-touch edge for capabilities.touch_observe.
 *
 * Header-only and pure, like fx_midi_filter.h, so tests/host can compile and
 * RUN it (tests/host/test_touch_observe.c). Its caller, shadow_midi.c, runs on
 * the SPI callback and cannot be built on the dev machine.
 *
 * A touch edge is a cable-0 (Move's own surface) Note On/Off (CIN 8 or 9) on knob notes 0-7 or jog note 9. Note 8 is the master-volume
 * knob's touch and belongs to the host. The cable test is the one that matters
 * most: cable 2 is an external USB keyboard, whose note 9 is a pitch.
 */
#ifndef TOUCH_OBSERVE_H
#define TOUCH_OBSERVE_H

#include <stdint.h>

static inline int touch_observe_is_edge(const uint8_t *slot8)
{
    if (!slot8) return 0;
    uint8_t cin = slot8[0] & 0x0F;
    uint8_t cable = (slot8[0] >> 4) & 0x0F;
    uint8_t type = slot8[1] & 0xF0;
    uint8_t note = slot8[2];
    if (cable != 0) return 0;
    if (cin != 0x08 && cin != 0x09) return 0;
    if (type != 0x80 && type != 0x90) return 0;
    return note <= 7 || note == 9;
}

#endif
