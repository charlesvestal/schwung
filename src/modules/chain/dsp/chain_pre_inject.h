/* Pure decision helper for Pre-mode inject of MIDI FX *tick* output.
 *
 * Extracted from v2_tick_midi_fx (chain_midi.c) so the skip logic is
 * unit-testable host-side without the full chain host. No side effects.
 *
 * Pre mode injects a chain MIDI FX's output into Move's MIDI_IN (cable 2) so
 * Move's native track instrument plays it additively. Two per-pitch trackers
 * gate what we send:
 *   pre_pad_held[128]       — >0: a pad is currently holding that pitch, so
 *                             Move already plays it via the cable-0 pad path.
 *   pre_injected_notes[128] — >0: we injected a note-on for that pitch and
 *                             haven't released it (Move is sounding our voice).
 */
#ifndef CHAIN_PRE_INJECT_H
#define CHAIN_PRE_INJECT_H

#include <stdint.h>

/* Returns 1 if a MIDI FX tick-output message should be injected into Move's
 * MIDI_IN (Pre mode), 0 to skip it. Only channel-voice messages (0x80-0xE0)
 * are injectable; the caller maps the USB-MIDI cable-in number. */
static inline int chain_pre_tick_should_inject(const uint8_t *pre_pad_held,
                                               const uint8_t *pre_injected_notes,
                                               const uint8_t *msg, int len) {
    if (len < 1) return 0;
    uint8_t type = msg[0] & 0xF0u;
    if (type < 0x80u || type > 0xE0u) return 0;  /* skip sysex/realtime */
    int is_note_on  = (type == 0x90u && len >= 3 && msg[2] > 0);
    int is_note_off = (type == 0x80u) || (type == 0x90u && len >= 3 && msg[2] == 0);
    uint8_t note = (len >= 2) ? msg[1] : 0u;
    if (note < 128u) {
        /* Skip note-ONs on pitches a pad is holding — Move plays them
         * natively via the cable-0 pad path. */
        if (is_note_on && pre_pad_held[note] > 0) return 0;
        /* Only inject a note-OFF for a pitch we actually injected a note-ON
         * for. A tick FX (arp) retriggers held-pad pitches with off/on pairs;
         * the ON is skipped above (Move drones the pad), so the OFF must be
         * skipped too — otherwise it (a) silences Move's held pad note and
         * (b) echoes back un-refcounted, re-entering the FX as a pad release
         * that drains its held notes (the arp plays once and stops). Genuinely
         * injected pitches (wide-arp octaves, chord intervals) have
         * pre_injected_notes[note] > 0, so their note-OFFs still release. */
        if (is_note_off && pre_injected_notes[note] == 0) return 0;
    }
    return 1;
}

#endif /* CHAIN_PRE_INJECT_H */
