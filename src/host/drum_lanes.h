/* drum_lanes.h -- play a Move DRUM track's clip as 16 monophonic, pitched
 * lanes into one Schwung slot.
 *
 * Move's drum rack is a 16-lane step sequencer already, and its "16 Pitches"
 * mode (Shift+Step 8) stores a pitch per note -- but NOT as a different note:
 * the note stays the PAD's own note and the pitch lives only in the note's
 * per-note PITCH expression, which never leaves Move as MIDI. So Schwung reads
 * the clip out of Move's document (move_model_playing_notes) and plays it
 * itself: pad k (note 36+k) is lane k, sent on MIDI channel k+1, at
 * base_note + pitch_offset. Storage, editing, copy/paste and undo stay Move's.
 *
 * dl_block is pure: no shim state, no clock, no I/O -- the caller supplies the
 * clip-time window of the block and a sink for the MIDI, so tests/host drives
 * it. RT-safe: bounded loops over preallocated state.
 *
 * PROTOTYPE switch, off by default: /data/UserData/schwung/drum_lanes.conf,
 * one line per Move track, "<track 1-4> <slot 1-4> [base note]". The slot
 * must not be the drum track's own number if Move's track is muted to silence
 * its kit -- slot mute follows Move's track mute. */
#pragma once
#include <stdint.h>
#include "move_model.h"

#define DL_LANES      16
#define DL_FIRST_PAD  36      /* Move's drum cells receive 36..51 by default */

typedef struct {
    uint8_t active;
    uint8_t note;             /* the note we sent, so the off matches it */
    double  remaining;        /* beats from the START of the next block to its end */
} dl_voice_t;

typedef struct {
    dl_voice_t v[DL_LANES];
    uint64_t   clip_id;       /* the clip the voices belong to (0 = none) */
} dl_track_t;

typedef void (*dl_emit_fn)(void *ctx, uint8_t status, uint8_t d1, uint8_t d2);

/* The clip's playable window in clip time: [ls, le), wrapping at le when
 * `loop` (the region before ls plays once, on the first pass). */
typedef struct { double ls, le; int loop; } dl_window_t;

/* Advance one block. `pos0` = clip position (beats) at the block's start, or
 * < 0 when there is no phase (stopped, unknown, a one-shot that ended): every
 * sounding lane is released and nothing new starts. `blk` = block length in
 * beats. A clip change (clip_id) releases the old clip's lanes first. */
void dl_block(dl_track_t *st, const mm_play_clip_t *clip, dl_window_t w,
              double pos0, double blk, int base_note,
              dl_emit_fn emit, void *ctx);

/* Release every sounding lane (a note-off each) and forget the clip. */
void dl_all_off(dl_track_t *st, dl_emit_fn emit, void *ctx);

/* ---- LIVE: pads played right now, from Move's engine (move_model_live_read)
 * The live stream is the SELECTED track's; the caller passes the slot that
 * track plays into (-1: not routed). Only NoteOns newer than the last one
 * seen start a note (ids only grow; records persist across blocks, so the
 * FIRST call primes the high-water mark and plays nothing). A note's
 * pitch is the PerNoteCC -2 carrying its id. Move's own NoteOff ends it, and
 * goes to the slot the note STARTED on, even after a track switch. */
typedef struct {
    uint8_t primed;               /* the first read only LEARNS the ids already there */
    int64_t last_on;              /* highest NoteOn id handled */
    int64_t id[DL_LANES];         /* the sounding note's id per lane, 0 = none */
    uint8_t note[DL_LANES];
    int8_t  slot[DL_LANES];
} dl_live_t;
typedef void (*dl_emit_slot_fn)(int slot, uint8_t status, uint8_t d1, uint8_t d2);
void dl_live_ingest(dl_live_t *st, const mm_live_rec_t *r, int n, int slot, int base_note,
                    dl_emit_slot_fn emit);
void dl_live_all_off(dl_live_t *st, dl_emit_slot_fn emit);

/* ---- prototype config (drum_lanes.conf), polled OFF the RT thread ---- */
#define DL_CONF_PATH "/data/UserData/schwung/drum_lanes.conf"
/* Per Move track: target slot 0..3 or -1 (off), and base note. Written by
 * dl_config_poll (shim worker), read by the RT side with atomic loads. */
extern int dl_cfg_slot[MM_TRACKS];
extern int dl_cfg_base[MM_TRACKS];
/* Parse a config text into the two arrays (pure; tests/host). */
void dl_config_parse(const char *text, int slot[MM_TRACKS], int base[MM_TRACKS]);
/* Re-read DL_CONF_PATH when it changed (absent = all off). ~1 Hz caller. */
void dl_config_poll(void);
