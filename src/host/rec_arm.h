/*
 * rec_arm.h — Move's Record button, decoded from its own LED.
 *
 * Move does not tell us it is recording. It tells its Record LED, on cable 0,
 * and we read that over its shoulder in the shim's existing MIDI_OUT scan.
 * Measured on hardware 2026-09-12; the capture and its three rules are in
 * docs/plans/2026-09-12-automation-lanes-design.md.
 *
 * Header-only and dependency-free for the same reason as recall_quantize.h and
 * transport_grid.h: the caller lives in the shim, which cannot be built on the
 * dev machine, so decision logic put there ships untested. This one is worse
 * than boundary maths if it is wrong -- a spurious RECORDING frame punches a
 * breakpoint into a lane the user never played, and an automation point that
 * should not exist is far harder to notice than one that is missing.
 *
 * Pure: no allocation, no I/O, no globals. The call site is the SPI callback.
 */
#ifndef REC_ARM_H
#define REC_ARM_H

#include <stdint.h>

/* Record is CC 86. NOT 118 -- schwung-spi's header documents 118 as "same
 * physical button as Sample", and 118 never appeared in the arm sequence. */
#define REC_ARM_CC 86

/* Move carries the LED ANIMATION in the channel nibble and the colour it
 * animates to in the value byte -- the same shape as the pad decode, where the
 * channel carried playing/queued and the colour byte carried nothing.
 * 0x06-0x0A pulse, 0x0B-0x0F blink (schwung-spi's SCHWUNG_ANIM_* vocabulary).
 * Which of the ten it is does not matter here: any of them means the button is
 * flashing, so no rate measurement and no colour comparison is needed. */
#define REC_ARM_ANIM_CH_FIRST 0x06
#define REC_ARM_ANIM_CH_LAST  0x0F

/* Full brightness. Read as a BRIGHTNESS, not as a palette index: the resting
 * state is static and non-zero too (122 and 124 were both observed), so
 * "static" alone cannot mean recording and the discriminator has to be the
 * button being at maximum. An exact hue comparison is the thing that breaks
 * the first time Move rethemes; "the user's record button is lit as bright as
 * it goes" is a design intent unlikely to invert. */
#define REC_ARM_FULL 127

typedef struct {
    /* Settled state. Written ONLY by rec_arm_frame_end(). */
    int recording;   /* solid at full brightness */
    int flashing;    /* an animation channel: armed, or counting in */
    int seen;        /* a CC 86 has arrived at least once, ever */

    /* Within-frame accumulator: the LAST CC 86 seen in the current frame.
     * Not a queue -- only the last one decides, so one slot is the whole
     * requirement. */
    int     have;
    uint8_t status;
    uint8_t d2;
} rec_arm_t;

/*
 * Offer one cable-0 MIDI_OUT message. Returns 1 if it was the Record LED.
 *
 * This does not decide anything, and that is the point. Move writes the base
 * colour statically and THEN applies the animation, so a single frame's burst
 * contains `static 127` immediately followed by `blink`. A decoder that acted
 * on each message as it arrived would report one frame of RECORDING every time
 * a count-in begins -- and one frame is enough to record a breakpoint.
 */
static inline int rec_arm_on_led(rec_arm_t *st, uint8_t status,
                                 uint8_t d1, uint8_t d2)
{
    if (!st) return 0;
    if ((status & 0xF0) != 0xB0) return 0;   /* a note carrying 86 is not this */
    if (d1 != REC_ARM_CC) return 0;
    st->have = 1;
    st->status = status;
    st->d2 = d2;
    return 1;
}

/*
 * Settle the frame. Returns the recording state.
 *
 * A frame with no CC 86 in it leaves everything alone. Move emits an LED
 * packet only when that LED changes, so most frames carry nothing at all, and
 * in set selection Record does nothing and lights nothing -- which means the
 * ABSENCE of an event is the disarmed state and must never be read as one.
 * Clearing on absence would drop a recording on the very next frame; persisting
 * the last seen state across a set or mode change is correct, inventing one is
 * not.
 */
static inline int rec_arm_frame_end(rec_arm_t *st)
{
    if (!st) return 0;
    if (!st->have) return st->recording;

    const uint8_t ch = (uint8_t)(st->status & 0x0F);
    st->have = 0;
    st->seen = 1;

    if (ch >= REC_ARM_ANIM_CH_FIRST && ch <= REC_ARM_ANIM_CH_LAST) {
        /* Flashing: armed and waiting, or queued and counting in. Never
         * recording -- lanes capture in exactly the window Move captures
         * notes, so holding Record and the count-in capture nothing. */
        st->flashing = 1;
        st->recording = 0;
        return 0;
    }

    st->flashing = 0;
    st->recording = (ch == 0 && st->d2 == REC_ARM_FULL) ? 1 : 0;
    return st->recording;
}

#endif /* REC_ARM_H */
