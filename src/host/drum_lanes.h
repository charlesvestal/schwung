/* drum_lanes.h -- a Move DRUM track's pads as 16 monophonic, pitched lanes,
 * sent as that track's MIDI in MPE form.
 *
 * Move's drum rack is a 16-lane step sequencer already, and its "16 Pitches"
 * mode (Shift+Step 8) gives each note a pitch -- but not as a different note:
 * the note stays the PAD's own note and the pitch lives only in a per-note
 * expression that Move never sends as MIDI (measured: with the track's MIDI
 * output on, a 16 Pitches press sends nothing at all). So Schwung reads the
 * notes out of Move -- the playing clip from its document, live presses from
 * its engine -- and sends them on as MIDI a synth can simply listen to:
 *
 *   lane k (pad, note 36+k) on MIDI channel k+1, one channel per pad (MPE)
 *   note-on  = CC 3 (1: a 16 Pitches note, 0: a plain hit)
 *            + pitch bend, 8192 + semitones * 8191/48 (MPE's default +-48;
 *              centre for a plain hit)
 *            + NoteOn(36+k, velocity)
 *   note-off = NoteOff(36+k)
 *
 * The note is never rewritten: the synth interprets note + pitch. CC 3 exists
 * because a plain hit and a 16 Pitches note at +0 carry the same bend, and a
 * module may treat them differently (Loop Lanes: whole loop vs one slice). An
 * MPE synth ignores it. The events go through the slots' own dispatch, so
 * receive/forward channel, transpose and MIDI FX apply -- to slots set to
 * Receive All only, since MPE uses every channel.
 *
 * The functions here are pure: no shim state, no clock, no I/O -- the caller
 * supplies the clip-time window and a sink, so tests/host drives them.
 * RT-safe: bounded loops over preallocated state.
 *
 * PROTOTYPE switch, off by default: /data/UserData/schwung/drum_lanes.conf,
 * one Move track number (1-4) per line. */
#pragma once
#include <stdint.h>
#include "move_model.h"

#define DL_LANES      16
#define DL_FIRST_PAD  36      /* Move's drum cells receive 36..51 by default */
#define DL_CC_PITCHED 3       /* 1: 16 Pitches note, 0: plain hit */
#define DL_PB_SEMIS   48.0    /* MPE's default per-note bend range */
/* The SEQUENCED path runs this much behind Schwung's MIDI clock. Measured on
 * hardware (2026-09-28, Skipback stems, Move kit vs slot synth on the same
 * steps): timed from the clock, drum-lane notes led Move's own audio by a
 * median 4.6 ms over 52 notes; LIVE notes (from Move's engine) landed within
 * +-2 ms and need nothing. What remains is block quantization, up to one
 * block (2.9 ms), because on_midi carries no in-block offset. */
#define DL_SEQ_LAG_MS 4.6

typedef void (*dl_emit_fn)(void *ctx, uint8_t status, uint8_t d1, uint8_t d2);

/* The three messages of a lane's note-on, and its note-off. */
void dl_emit_on(dl_emit_fn emit, void *ctx, int lane, int velocity, int has_pitch, double semis);
void dl_emit_off(dl_emit_fn emit, void *ctx, int lane);

/* ---- SEQUENCED: the track's playing clip ---- */
typedef struct {
    uint8_t active;
    double  remaining;        /* beats from the START of the next block to its end */
} dl_voice_t;

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
 * beats. A clip change (clip_id) releases the old clip's lanes first. */
void dl_block(dl_track_t *st, const mm_play_clip_t *clip, dl_window_t w,
              double pos0, double blk, dl_emit_fn emit, void *ctx);

/* Release every sounding lane (a note-off each) and forget the clip. */
void dl_all_off(dl_track_t *st, dl_emit_fn emit, void *ctx);

/* ---- LIVE: pads played right now, from Move's engine (move_model_live_read)
 * The live stream is the SELECTED track's; `routed` says whether that track
 * has lanes on. Only NoteOns newer than the last one seen start a note (ids
 * only grow and records persist across blocks, so the FIRST call primes the
 * high-water mark and plays nothing). A note's pitch is the PerNoteCC -2
 * carrying its id. Move's own NoteOff ends it -- even after a track switch. */
typedef struct {
    uint8_t primed;               /* the first read only LEARNS the ids already there */
    int64_t last_on;              /* highest NoteOn id handled */
    int64_t id[DL_LANES];         /* the sounding note's id per lane, 0 = none */
} dl_live_t;
void dl_live_ingest(dl_live_t *st, const mm_live_rec_t *r, int n, int routed,
                    dl_emit_fn emit, void *ctx);
void dl_live_all_off(dl_live_t *st, dl_emit_fn emit, void *ctx);

/* ---- prototype config (drum_lanes.conf), polled OFF the RT thread ---- */
#define DL_CONF_PATH "/data/UserData/schwung/drum_lanes.conf"
extern int dl_cfg_on[MM_TRACKS];          /* written by dl_config_poll, read with atomics */
/* One track number (1-4) per line; anything after it is ignored. */
void dl_config_parse(const char *text, int on[MM_TRACKS]);
/* Re-read DL_CONF_PATH when it changed (absent = all off). ~1 Hz caller. */
void dl_config_poll(void);
