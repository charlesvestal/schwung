/*
 * clip_state — which clip is playing on each Move track, and where in it.
 *
 * Move's sequencer emits no CC, but its cable-0 MIDI_OUT LED stream carries
 * clip identity, launch and playhead. See
 * docs/plans/2026-09-12-clip-awareness-design.md for the measurements.
 *
 * THE THREE RULES THAT ARE NOT OBVIOUS
 * ------------------------------------
 * 1. The pad LED CHANNEL is the state: ch 9 = playing, ch 14 = queued. The
 *    colour byte is not. `move_note_led_state[]` is indexed by note and keeps
 *    only the last colour, so it COLLAPSES the channel -- it throws away the
 *    whole signal one line after the scan sees it. Decode at the scan.
 *
 * 2. A BARE ch-9 ON IS A REFRESH, NOT A LAUNCH. Entering Session mode makes
 *    Move re-emit the grid, including a ch-9 ON for every clip already
 *    playing (measured: four tracks at one pulse). Anchoring on that
 *    re-anchors every playing track and destroys the phase of everything that
 *    was correct -- silently, and only when the user visits Session view. A
 *    real launch is ch-14 QUEUED then ch-9 ON on the same pad.
 *
 * 3. A TRANSPORT START RE-ANCHORS EVERYTHING TO ZERO. Measured 24/24 exact
 *    against the step playhead. So the naive (pulses/24) mod L is right after
 *    a Start, and the anchor below is the exception path for a clip launched
 *    mid-playback -- not the main mechanism.
 *
 * IDENTITY AND ANCHOR ARE SEPARATELY VALID. A refresh gives identity with no
 * anchor; attaching mid-session gives neither. Collapsing them is how a lane
 * binds to the right clip at the wrong phase. Nothing here returns a default:
 * an unknown anchor is NOT phase 0.
 *
 * RT: clip_state_on_led() is called from the SPI callback. Table writes only.
 */
#ifndef CLIP_STATE_H
#define CLIP_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLIP_TRACKS 4
#define CLIP_SLOTS  8

/* Move's session grid: note = 92 - 8*track + slot, over notes 68..99. */
#define CLIP_PAD_NOTE_MIN 68
#define CLIP_PAD_NOTE_MAX 99

/* The two channels that carry meaning. Anything else is base colour. */
#define CLIP_UI_MODE_SESSION 1

#define CLIP_CH_PLAYING 9
#define CLIP_CH_QUEUED  14

typedef struct {
    int      identity_valid;  /* 0 = we do not know what this track is doing */
    int      clip_slot;       /* 0..7, or -1 for "nothing playing" */
    int      anchor_valid;    /* 0 = phase UNKNOWN. Not zero. Unknown. */
    uint32_t anchor_pulse;    /* shadow_transport_pulses at the clip's step 0 */
} clip_track_state_t;

typedef struct {
    clip_track_state_t tracks[CLIP_TRACKS];
    /* Per track, the slot with a pending ch-14 QUEUED, or -1. This is the
     * whole of rule 2: only a ch-9 ON that follows a queue on the same pad
     * may write an anchor. */
    int      queued_slot[CLIP_TRACKS];
    /* Per track: we WATCHED its playing clip stop, and nothing has started
     * since. A ch-9 ON after that is a clip we saw begin, so it may anchor --
     * the same standing as a queued launch, and for the same reason. Without
     * it, choosing a clip on a stopped track (which is how a set is started)
     * left that very track unanchored for good. */
    int      saw_stop[CLIP_TRACKS];
    uint32_t last_pulse;
    int      seen_pulse;
} clip_state_t;

/* Map a pad note to (track, slot). Returns 0 if the note is not a grid pad. */
int clip_pad_decode(int note, int *out_track, int *out_slot);

void clip_state_reset(clip_state_t *st);

/* MIDI Start (0xFA). Every playing clip returns to its top in lockstep with
 * the pulse counter, so this anchors them all at 0.
 *
 * This MUST be driven by the real 0xFA. An earlier version inferred it from
 * the pulse counter going backwards, which only ever detects a RE-start: after
 * a reboot the counter is already 0 and the first Start also begins at 0, so
 * nothing moved backwards and three of four tracks sat unanchored through a
 * full minute of playback. The unit tests passed throughout -- they fed a
 * synthetic backwards step, which is the transition the device does not
 * actually make. */
void clip_state_on_transport_start(clip_state_t *st);

/* Feed one cable-0 MIDI_OUT event, with the transport pulse count at the time
 * it was seen. Non-pad and non-note events are ignored, so this is safe to
 * call for every event in the scan.
 *
 * Detects a transport restart itself, from the pulse counter going backwards
 * -- the LED scan never sees 0xFA, and a counter reset is the same fact. */
/* `running` is the transport state. It is load-bearing, not a nicety:
 *
 *   TRANSPORT PLAYING  ch 9 means PLAYING.
 *   STOPPED / JUST LOADED  ch 9 means SELECTED. A set restores a selected
 *   clip per track with nothing sounding, and pressing one of them both
 *   selects it AND starts the transport.
 *
 * So a selection is not a launch and must never set an anchor -- phase is
 * meaningless while stopped anyway, and the Start that follows anchors every
 * selected track to 0, which is exactly right because they all begin together. */
/* `ui_mode` is shadow_control->move_ui_mode: 1 = Session, 2 = Note,
 * 3 = Set Overview, 0 = not yet known.
 *
 * PAD EVENTS ARE ONLY CLIP STATE IN SESSION MODE. This is a hard gate, not a
 * refinement. In Set Overview the pads show SETS, and at boot the mode is
 * simply unknown -- and Move lights those grids on the same notes and the same
 * channel 9. Measured: of 11 ch-9 events in a 131,699-row capture, the two
 * that named a clip nobody was playing arrived at mode 3 and mode 0, and every
 * correct one at mode 1. Ungated, that put a track on the wrong clip AND
 * anchored it, which is the confidently-wrong answer this whole design exists
 * to refuse.
 *
 * (The colour differs too -- the spurious ones were dim, d2=9/11, against
 * d2=122 for real state. That is not used: a velocity is a colour, and keying
 * on an exact value would break the first time Move retheme s anything.) */
void clip_state_on_led(clip_state_t *st, uint8_t status, uint8_t d1,
                       uint8_t d2, uint32_t pulses, int running, int ui_mode);

/* Phase in beats from the clip's LOOP START, or 0 if unknown (check the
 * return value; the out param is untouched on failure).
 *
 * loop_start/loop_len come from Song.abl. The loop is NOT always at 0.0. */
int clip_phase_beats(const clip_track_state_t *t, uint32_t pulses,
                     double loop_start, double loop_len, double *out_beats);

/* The live table, or NULL before the first cable-0 scan. Worker-thread read
 * of callback-written data: fields are independent ints, a torn read is
 * informational, and nothing here is used to gate audio. */
const clip_state_t *clip_state_current(void);

/* The same table, writable, for the transport hook. Both callers are on the
 * SPI callback. NULL before the first cable-0 scan. */
clip_state_t *clip_state_mutable(void);

#ifdef __cplusplus
}
#endif
#endif /* CLIP_STATE_H */
