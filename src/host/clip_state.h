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

/* The two channels that carry meaning. Anything else is base colour.
 *
 * THESE ARE NOT TWO MAGIC NUMBERS -- THEY ARE ANIMATIONS. Move puts the LED
 * ANIMATION in the channel nibble: 9 = 0x09 = SCHWUNG_ANIM_PULSE_4TH and
 * 14 = 0x0E = SCHWUNG_ANIM_BLINK_4TH (schwung-spi's vocabulary; the same
 * 0x06-0x0A pulse / 0x0B-0x0F blink split rec_arm.h decodes for the Record
 * button, where an animation channel IS "flashing"). So "ch 9 = playing,
 * ch 14 = queued" is really PULSING = PLAYING, BLINKING = QUEUED -- the
 * animation is the state, and the user reads the very same signal off the
 * hardware: a queued clip flashes.
 *
 * Worth holding on to because it says WHY the decoder can trust the channel
 * and must ignore the colour, and because it predicts the next surface: any
 * Move control whose state is an animation is decodable the same way. */
/* How long after a Start a first-sighting may still be attributed to that
 * Start rather than to a launch. Move repaints the grid within a beat or two;
 * beyond a bar, a clip appearing is someone pressing a pad. */
#define CLIP_START_GRACE_PULSES 96   /* one bar at 4/4 */
/* How long a ch-9 OFF may sit undecided before it is taken as a real stop.
 *
 * Move drops the outgoing clip's PULSING when a replacement is QUEUED, not
 * when the clip stops, so an OFF is ambiguous at the moment it arrives -- and
 * the queue that would disambiguate it arrives AFTER it (capture: seq 369 OFF
 * then seq 371 ch-14). See the pending_off fields below.
 *
 * One bar at 4/4, matching CLIP_START_GRACE_PULSES, because the queue follows
 * the OFF within a frame or two on the measured capture and a whole bar is
 * generous by two orders of magnitude -- but only ONE observation puts the two
 * in the same frame, so the window is sized for the ordering to survive
 * whatever frame the queue lands in rather than fitted to that measurement.
 *
 * THE TWO ERRORS ARE NOT EQUAL, WHICH IS WHY IT ERRS LONG. Too short
 * reinstates the user's bug: identity clears while the clip is still audible
 * and its automation stops mid-bar. Too long leaves identity naming a clip
 * that genuinely stopped, so a lane keeps driving a synth nobody can hear --
 * inaudible, self-correcting on the next event, and bounded by this constant.
 * A lane that stops early is the defect; a lane that runs on into silence is
 * not. */
#define CLIP_OFF_GRACE_PULSES 96   /* one bar at 4/4 */

#define CLIP_ANCHOR_NONE    0
#define CLIP_ANCHOR_START   1   /* MIDI Start: everything begins together   */
#define CLIP_ANCHOR_LAUNCH  2   /* a launch we witnessed                    */
#define CLIP_ANCHOR_DERIVED 3   /* solved from Move's playhead + page       */

#define CLIP_UI_MODE_SESSION 1

#define CLIP_CH_PLAYING 9
#define CLIP_CH_QUEUED  14

typedef struct {
    int      identity_valid;  /* 0 = we do not know what this track is doing */
    int      clip_slot;       /* 0..7, or -1 for "nothing playing" */
    int      anchor_valid;    /* 0 = phase UNKNOWN. Not zero. Unknown. */
    uint32_t anchor_pulse;    /* shadow_transport_pulses at the clip's step 0 */
    /* Where the anchor came from. Kept because a DERIVED anchor is computed
     * from Move's own playhead, which is also what the phase check scores
     * against -- so scoring a derived anchor is partly circular and must be
     * told apart from an anchor a Start or a launch produced independently. */
    int      anchor_source;   /* CLIP_ANCHOR_* */
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
    /* Per track: a Start happened while this track had NO clip, so whatever
     * comes up next began AT that Start, not when we noticed it. Consumed by
     * the first ch-9 ON. Without it a track that fell silent just before the
     * Start could never anchor again -- observed on hardware, and the cause
     * was on_transport_start clearing saw_stop, i.e. destroying the very
     * evidence that would have rescued it. */
    int      pending_start[CLIP_TRACKS];
    /* Per track: a ch-9 OFF we have seen but NOT acted on, and the pulse it
     * arrived at. -1 for none.
     *
     * HARDWARE: "I'm playing clip A, I press clip B and the automation for
     * clip A stops. then clip B starts playing with automation." Clip A keeps
     * SOUNDING to the launch-quantize boundary; Move turns its pulsing off at
     * QUEUE time. Measured 86 pulses -- ~3.6 beats at 4/4 -- between the OFF
     * and the new clip's ch-9 ON. Clearing identity at the OFF released the
     * lane's position gate there, so automation stopped beats before the audio
     * did.
     *
     * IT CANNOT BE GATED ON "IS A LAUNCH QUEUED", because the OFF arrives
     * FIRST (seq 369 then 371) -- nothing is queued yet when it is decoded.
     * So the verdict is deferred and resolved by what happens next:
     *
     *   ch-14 on this track      -> REPLACED. Discard; identity and anchor
     *                               stay with the clip that is still
     *                               sounding until the new one's ch-9 ON.
     *   ch-9 ON on another slot  -> REPLACED, and witnessed (see saw_stop).
     *   ch-9 ON on the SAME slot -> the clip never stopped; keep its anchor.
     *   a transport Start        -> a hard resync; commit it.
     *   CLIP_OFF_GRACE_PULSES    -> STOPPED. Commit: identity clears and
     *                               saw_stop records the witnessed silence.
     *
     * Only the STOPPED paths set saw_stop. A replacement is not a witnessed
     * silence, and the ch-14/ch-9 pair is the stronger evidence rule 2 wants;
     * setting saw_stop there would let a later BARE ch-9 ON anchor without a
     * queue, which is exactly what rule 2 refuses. */
    int      pending_off_slot[CLIP_TRACKS];
    uint32_t pending_off_pulse[CLIP_TRACKS];
    uint32_t last_pulse;
    int      seen_pulse;
    /* Last UI mode the scan reported. Recorded even when the event is
     * rejected, because "what mode does Schwung think Move is in" is the
     * first thing to check when the grid reads wrong -- the gate is silent
     * by design, so without this a rejected event is indistinguishable from
     * no event at all. */
    int      last_ui_mode;
} clip_state_t;

/* Map a pad note to (track, slot). Returns 0 if the note is not a grid pad. */
int clip_pad_decode(int note, int *out_track, int *out_slot);

void clip_state_reset(clip_state_t *st);

/* Solve a track's anchor from a single playhead sighting.
 *
 * The playhead says WHERE the clip is right now; the page says which bar the
 * lit button belongs to. Together they give an absolute position, and the
 * anchor is that subtracted from the current pulse:
 *
 *     step   = (bar - 1) * 16 + idx
 *     anchor = pulses - step * step_beats * 24
 *
 * This needs no Start and no witnessed launch, which is the point: loading a
 * set while the transport keeps running produces neither, and before this
 * every track sat at "phase unknown" indefinitely -- identity known, phase
 * unknowable. Detecting the set switch does not help, because our detection
 * is a ~1.4 s poll and anchoring to "when we noticed" is wrong by up to most
 * of a bar.
 *
 * Only fills a MISSING anchor. An anchor from a Start or a launch is
 * independent evidence and is never overwritten by one derived from the
 * instrument we check against.
 *
 * Returns 1 if an anchor was set. */
int clip_state_derive_anchor(clip_track_state_t *tr, uint32_t pulses,
                             int bar, int playhead_idx, double step_beats,
                             double loop_start, double loop_len);

/* Solve the set's TRUE common start, given one playhead sighting and a rough
 * idea of when the set loaded.
 *
 * Every clip in a set begins together, so there is ONE start pulse and every
 * track's phase is (pulses - start)/24 mod its own loop. The difficulty is
 * that neither input gives it alone:
 *
 *   - the set-change detection is a ~1.4 s poll, so it brackets the start to
 *     about +/-67 pulses (~2.8 beats at 120 BPM) -- far too coarse to anchor;
 *   - a playhead sighting is exact but gives the start only MODULO that
 *     track's loop.
 *
 * A congruence plus a bracket pins it uniquely, provided the loop is longer
 * than the bracket is wide. Then the answer is the real start and EVERY track
 * can use it whatever its loop length -- which is what makes this better than
 * sharing a loop-boundary anchor, where a track whose loop did not divide the
 * source's had to be left unknown.
 *
 * Refuses when the loop is too short to disambiguate: with a 4-beat loop the
 * bracket spans more than one loop and two candidates are equally good, so
 * there is no answer to give.
 *
 * Returns 1 and writes *out_start on success. */
int clip_state_solve_common_start(uint32_t pulses, double pos_beats,
                                  double loop_len, uint32_t coarse_start,
                                  uint32_t bracket_pulses,
                                  uint32_t *out_start);

/* Apply a solved common start to every track that lacks an anchor. */
int clip_state_apply_common_start(clip_state_t *st, uint32_t start);

/* Share one track's anchor with the others, where that is SOUND.
 *
 * On a set load every clip begins together, so they share a start pulse and
 * one anchor would serve all. But a DERIVED anchor is not the start: winding
 * back from a single playhead sighting can only reach the current loop, so it
 * is
 *
 *     derived = start + k * src_loop * 24     for an unknown k
 *
 * which is equivalent for the source track (its phase is mod src_loop) and
 * equivalent for another track only when k * src_loop is a whole number of
 * ITS loops for every k -- i.e. when its loop DIVIDES the source's.
 *
 * 8/16/16/4 satisfies that, which is exactly why the bug would hide; a 20- or
 * 12-beat loop does not, and the error would be a silent fraction of a bar.
 * So the divisibility is checked and a track that fails it stays unknown.
 *
 * Only fills MISSING anchors, and marks them derived so nothing scores them.
 * Returns how many tracks were given an anchor. */
int clip_state_share_anchor(clip_state_t *st, int src_track,
                            const double *loop_len);

/* Anchor any track that has identity but no anchor, and for which a Start is
 * still pending within the grace window.
 *
 * Exists because identity can arrive LATE and from a different source. A set
 * load restarts the transport immediately, but seeding from Song.abl runs on
 * a ~1.4 s poll -- so 0xFA finds nothing to anchor, the seed lands afterwards
 * with no anchor, and every track sits at "phase unknown" until the next
 * Start. Observed on hardware. clip_state_on_led consumes pending_start the
 * same way; this is the path for identity that did not come from an LED. */
void clip_state_anchor_pending(clip_state_t *st, uint32_t pulses, int running);

/* Commit any deferred ch-9 OFF whose grace has run out (see pending_off_slot).
 *
 * Time is the only thing that can resolve "the clip really stopped", and no
 * LED event announces it -- Move said all it was going to say at the OFF. So
 * this is driven per frame from the cable-0 scan, beside rec_arm_frame_end(),
 * and clip_state_on_led() also runs it on the way in so an event arriving past
 * the grace settles the verdict before it is interpreted. A pulse count that
 * has gone BACKWARDS (a restart) commits too: the deferral cannot span two
 * timelines.
 *
 * RT: called from the SPI callback. Table writes only. */
void clip_state_expire_pending_off(clip_state_t *st, uint32_t pulses);

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

/* ============================================================================
 * Phase check — does our computed phase agree with Move's own playhead?
 *
 * Everything else verifies that phase COUNTS and WRAPS at the right length.
 * That is not the same as being correct: a lane anchored a beat out counts
 * and wraps perfectly and records everything in the wrong place.
 *
 * Move's step playhead is an independent measurement of the same quantity.
 * The comparison is page-independent, which is what makes it usable without
 * the page oracle:
 *
 *     playhead_idx == floor(pos_in_loop / step_resolution) mod 16
 *
 * because the index IS the step within whatever page is displayed. A one-beat
 * error shows up as a mismatch of 4, not as a rounding wobble.
 * ========================================================================= */
#define CLIP_PH_RING 64
typedef struct {
    uint8_t  idx;       /* 0..15, the lit step button */
    uint32_t pulses;
} clip_playhead_ev_t;

/* Producer: the SPI callback. Consumer: the worker. A store and an index. */
void clip_playhead_record(uint8_t idx, uint32_t pulses);
int  clip_playhead_take(clip_playhead_ev_t *out, int max);

/* The live table, or NULL before the first cable-0 scan. Worker-thread read
 * of callback-written data: fields are independent ints, a torn read is
 * informational, and nothing here is used to gate audio. */
const clip_state_t *clip_state_current(void);

/* The same table, writable, for the transport hook. Both callers are on the
 * SPI callback. NULL before the first cable-0 scan. */
clip_state_t *clip_state_mutable(void);

/* Which track Move has selected (0..3), or -1 if not known. The step editor
 * shows THIS track, so it is the only track whose page a "Bar N" describes
 * and the only one a bar-level comparison can speak to. */
int clip_selected_track(void);

#ifdef __cplusplus
}
#endif
#endif /* CLIP_STATE_H */
