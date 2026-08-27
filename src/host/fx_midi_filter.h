/*
 * What MIDI is allowed to reach an audio FX.
 *
 * Two predicates, both pure — no allocation, no I/O, no locks — because every
 * call site is the SCHED_FIFO 90 SPI callback. Header-only for the same reason
 * as master_fx_key.h: so tests/host can compile and RUN them natively
 * (tests/host/test_fx_midi_filter.c). The translation units that call them
 * (schwung_shim.c, shadow_midi.c) cannot be built on the dev machine, which is
 * exactly how filtering like this ends up shipped untested.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS
 *
 * Audio FX are fed from THREE places, and none of them filtered anything
 * beyond "is it a note, and is d1 >= 10":
 *
 *   schwung_shim.c   MIDI_IN cable 0 (Move's own surface)  notes only
 *   shadow_midi.c    shadow_chain_dispatch_midi_to_slots   ALL voice messages
 *   shadow_midi.c    shadow_dispatch_direct_external_midi  cable-2 THRU
 *
 * The `d1 >= 10` guard exists only to drop the capacitive knob-touch notes
 * 0-9. It was never a statement about what is musical input. So on Move's own
 * surface, STEP buttons (notes 16-31) and TRACK buttons (notes 40-43) reached
 * every loaded audio FX as if they were played notes. Found with an FX whose
 * note handler fires a one-shot action (a granular re-slice): in Master FX it
 * fired on essentially any button press. The ducker had the same exposure and
 * merely read as "sensitive".
 *
 * The two predicates fix different halves of that, and the split matters:
 *
 *   move_surface_note_is_pad  is for cable 0 ONLY, where a note number is a
 *                             physical CONTROL IDENTITY rather than a pitch.
 *   fx_midi_channel_accepts   is for all three sites, where the channel is
 *                             meaningful.
 *
 * Do not apply the note-range guard to the external sites. There a note
 * number IS a pitch, and range-filtering an external keyboard down to 68-99
 * would silence five octaves of it.
 * ---------------------------------------------------------------------------
 */
#ifndef FX_MIDI_FILTER_H
#define FX_MIDI_FILTER_H

#include <stdint.h>

/* Listen on every channel. The DEFAULT, deliberately: this setting shipped
 * into a world where Master FX already received everything, so any other
 * default silently breaks every existing sidechain setup — with no way for
 * the user to connect a dead ducker to a setting they never saw. */
#define FX_MIDI_CHANNEL_ALL (-1)

/* Move's cable-0 surface note map (CLAUDE.md, "Move Hardware MIDI"):
 *   0-9    capacitive knob touch
 *   16-31  step buttons
 *   40-43  track buttons
 *   68-99  pads
 * Only the pads are musical input. */
#define MOVE_SURFACE_PAD_LOW  68
#define MOVE_SURFACE_PAD_HIGH 99

/*
 * Is this cable-0 note number a pad?
 *
 * Callers use this in place of the old `d1 >= 10`. Note that a channel
 * setting CANNOT substitute for this: pads and step buttons arrive on the
 * same hardware surface, so no channel value separates them.
 */
static inline int move_surface_note_is_pad(uint8_t note)
{
    return note >= MOVE_SURFACE_PAD_LOW && note <= MOVE_SURFACE_PAD_HIGH;
}

/*
 * Does `status` pass the configured listen channel?
 *
 * `channel_setting` is 0-based (0 = MIDI channel 1) to match the status low
 * nibble; the UI shows 1-16. FX_MIDI_CHANNEL_ALL disables the filter.
 *
 * Two deliberate pass-throughs:
 *
 *   System messages (0xF0-0xFF) have no channel — their low nibble is a
 *   message id, so masking it would drop MIDI clock on 15 of 16 settings.
 *   Clock drives arps and synced LFOs; that is not this setting's business.
 *
 *   An out-of-range stored value fails OPEN. A corrupt config should not
 *   silently mute every audio FX on the device, which is a fault with no
 *   visible cause and no obvious cure.
 */
static inline int fx_midi_channel_accepts(int channel_setting, uint8_t status)
{
    if (channel_setting < 0 || channel_setting > 15) return 1;  /* All */
    if (status < 0x80) return 1;                 /* not a status byte */
    if ((status & 0xF0) == 0xF0) return 1;       /* system: no channel */
    return (status & 0x0F) == channel_setting;
}

#endif /* FX_MIDI_FILTER_H */
