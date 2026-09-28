/* drum_lanes.h -- the 16 Pitches notes Move never sends, added to a Move
 * drum track's own MIDI output.
 *
 * Move's drum rack is a 16-lane step sequencer already, and its "16 Pitches"
 * mode (Shift+Step 8) gives each note a pitch -- but not as a different note:
 * the note stays the PAD's own note and the pitch lives only in a per-note
 * expression (Live's MPE note PitchBend), which Move never sends as MIDI. With
 * the track's MIDI output on, a plain hit goes out as the pad's note and a
 * 16 Pitches note goes out as nothing at all.
 *
 * So Schwung reads the pitched notes out of Move -- the playing clip from its
 * document, live presses from its engine -- and supplies exactly those, on the
 * track's own output channel, through the slots' ordinary dispatch (as if Move
 * had sent them: receive channel, forward remap, transpose and MIDI FX apply):
 *
 *   note-on  = CC 3 = 1                   "the next note is a 16 Pitches note"
 *            + pitch bend, 8192 + semitones * 8191/48 (MPE's +-48)
 *            + NoteOn(pad note, velocity)
 *            + CC 3 = 0                   so Move's next plain hit reads plain
 *   note-off = NoteOff(pad note) + pitch bend centre
 *
 * A module that knows the contract latches CC 3 and the bend at NoteOn (Loop
 * Lanes: whole loop vs one slice); a plain synth just hears the pad's note,
 * bent while it is held. Several pads may sound at once, each latched at its
 * own NoteOn; one channel's bend is last-wins for a synth that does not latch.
 *
 * A track with its MIDI output OFF sends nothing, so there Schwung sends the
 * plain hits too, on channel track+1 (the slot N / track N pairing).
 *
 * The functions here are pure: no shim state, no clock, no I/O -- the caller
 * supplies the clip-time window and a sink, so tests/host drives them.
 * RT-safe: bounded loops over preallocated state.
 *
 * Off by default: a slot turns it on (Slot Settings -> Drum MPE). */
#pragma once
#include <stdint.h>
#include "move_model.h"

#define DL_LANES      16
#define DL_FIRST_PAD  36      /* Move's drum cells receive 36..51 by default */
#define DL_CC_PITCHED 3       /* 1: the next note is a 16 Pitches note; 0 after it */
#define DL_PB_SEMIS   48.0    /* MPE's default per-note bend range */
/* The SEQUENCED path runs this much behind Schwung's MIDI clock. Measured on
 * hardware (2026-09-28, Skipback stems, Move kit vs slot synth on the same
 * steps): timed from the clock, drum-lane notes led Move's own audio by a
 * median 4.6 ms over 52 notes; LIVE notes (from Move's engine) landed within
 * +-2 ms and need nothing. What remains is block quantization, up to one
 * block (2.9 ms), because on_midi carries no in-block offset. */
#define DL_SEQ_LAG_MS 4.6

typedef void (*dl_emit_fn)(void *ctx, uint8_t status, uint8_t d1, uint8_t d2);

/* Where a track's lanes go: its MIDI channel (0-15), and whether plain hits
 * are ours to send too (the track's own output is off). */
typedef struct { int ch; int plain; } dl_out_t;
/* From Move's midiOutputEndpoint (-1 = off) for track `t`. */
dl_out_t dl_out_for(int t, int midi_out_ep);

/* A note-on (the four messages above; a plain hit is the NoteOn alone), and
 * its note-off. */
void dl_emit_on(dl_emit_fn emit, void *ctx, int ch, int note, int velocity, int has_pitch, double semis);
void dl_emit_off(dl_emit_fn emit, void *ctx, int ch, int note, int has_pitch);

/* One sounding lane, as we sent it. */
typedef struct {
    uint8_t active;
    uint8_t pitched;
    uint8_t ch;               /* the channel its note-on went to: the off follows it */
    double  remaining;        /* SEQUENCED: beats from the START of the next block to its end */
} dl_voice_t;

/* ---- SEQUENCED: the track's playing clip ---- */
typedef struct {
    dl_voice_t v[DL_LANES];
    uint64_t   clip_id;       /* the clip the voices belong to (0 = none) */
} dl_track_t;

/* The clip's playable window in clip time: [ls, le), wrapping at le when
 * `loop` (the region before ls plays once, on the first pass). */
typedef struct { double ls, le; int loop; } dl_window_t;

/* Advance one block. `pos0` = clip position (beats) at the block's start, or
 * < 0 when there is no phase (stopped, unknown, a one-shot that ended): every
 * sounding lane is released and nothing new starts. `blk` = block length in
 * beats. A clip change (clip_id) releases the old clip's lanes first.
 * A plain note that is not ours to send still ENDS a pitched one on its lane
 * (Move's pads are monophonic): by a bend reset, not a NoteOff -- Move's own
 * NoteOn for the same pad note is already on its way, and an off would kill it. */
void dl_block(dl_track_t *st, const mm_play_clip_t *clip, dl_window_t w,
              double pos0, double blk, dl_out_t out, dl_emit_fn emit, void *ctx);

/* Release every sounding lane (a note-off each) and forget the clip. */
void dl_all_off(dl_track_t *st, dl_emit_fn emit, void *ctx);

/* ---- LIVE: pads played right now, from Move's engine (move_model_live_read)
 * The live stream is the SELECTED track's; `routed` says whether that track
 * has lanes on. Only NoteOns newer than the last one seen start a note (ids
 * only grow and records persist across blocks, so the FIRST call primes the
 * high-water mark and plays nothing). A note's pitch is the PerNoteCC -2
 * carrying its id. Move's own NoteOff ends it -- even after a track switch. */
typedef struct {
    uint8_t    primed;            /* the first read only LEARNS the ids already there */
    int64_t    last_on;           /* highest NoteOn id handled */
    int64_t    id[DL_LANES];      /* the sounding note's id per lane, 0 = none */
    dl_voice_t v[DL_LANES];
} dl_live_t;
void dl_live_ingest(dl_live_t *st, const mm_live_rec_t *r, int n, int routed, dl_out_t out,
                    dl_emit_fn emit, void *ctx);
void dl_live_all_off(dl_live_t *st, dl_emit_fn emit, void *ctx);

/* ---- which tracks: the slots that asked (Slot Settings -> Drum MPE) ----
 * A track's lanes run when some slot with Drum MPE on hears the track's
 * channel -- its MIDI output channel, or track+1 when that is off (dl_out_for)
 * -- or listens to All. `slot_rx`: 0-15, or -1 = All. */
void dl_tracks_wanted(int nslots, const int *slot_rx, const int *slot_on,
                      const int track_ch[MM_TRACKS], int on[MM_TRACKS]);
