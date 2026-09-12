/*
 * clip_regions — per-clip loop geometry and the restored selection, read from
 * Move's Song.abl.
 *
 * WHY THIS EXISTS
 * ---------------
 * The LED stream only says anything in Session mode, so until the user visits
 * that screen Schwung knows nothing -- and if MIDI Start arrives first, every
 * track is unanchored permanently, because there was no identity to anchor.
 * Seeding from the file closes that: identity is present BEFORE 0xFA, so the
 * Start anchors everything to 0 the way it should.
 *
 * It is also the only source of LOOP LENGTH, without which there is no phase,
 * only elapsed beats.
 *
 * PRECEDENCE: THE FILE SEEDS, THE LEDS OVERRIDE. Song.abl is save-time state
 * and can be stale. A live LED observation must always win, and a file read
 * must never overwrite something actually observed.
 *
 * The loop is NOT always at 0.0 -- a clip whose loop starts at bar 3 is
 * normal -- so phase is relative to loop_start, never to the clip start.
 *
 * RT: none of this runs on the SPI callback. Parsing is worker-thread only.
 */
#ifndef CLIP_REGIONS_H
#define CLIP_REGIONS_H

#include "clip_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    exists;       /* a clip occupies this slot at all */
    int    is_playing;   /* the file's isPlaying -- after a load this is the
                          * RESTORED SELECTION, not something sounding */
    double loop_start;   /* beats */
    double loop_len;     /* beats; 0 if unknown */
    /* Where this clip's step editor was left, in beats from the clip start.
     * PER CLIP -- switching track shows that track's clip at its own
     * remembered page, which is why a single global "current bar" cannot
     * work: the bar you last heard announced belongs to whichever track was
     * last paged, not to the one on screen now. */
    double scroll_beats;
    int    have_scroll;
} clip_region_t;

typedef struct {
    clip_region_t slots[CLIP_TRACKS][CLIP_SLOTS];
    int    valid;            /* 0 = nothing parsed; do not use the contents */
    double step_resolution;  /* beats per step, e.g. 0.25 for 1/16 */
} clip_regions_t;

/* Parse a Song.abl. Returns 1 on success. On failure `out->valid` is 0 and
 * the caller must treat every field as unknown -- a partial parse is not a
 * usable answer. */
int clip_regions_parse_file(const char *path, clip_regions_t *out);

/* Same, over a buffer already in memory (what the tests drive). */
int clip_regions_parse(const char *json, size_t len, clip_regions_t *out);

/* Seed identity for tracks we have NOT observed. Never touches a track whose
 * identity came from the LED stream, and never sets an anchor: the file says
 * what is selected, not when it started. */
void clip_regions_seed_state(const clip_regions_t *rg, clip_state_t *st);

/* Does the geometry that scoring depends on actually differ? Move saves
 * periodically, so a re-parse is common and mostly changes nothing -- keying
 * a tally reset on "the file was written" wipes it every save and the tally
 * never accumulates. Only a real change to a clip's existence or its loop
 * invalidates samples taken before it. */
int clip_regions_geometry_differs(const clip_regions_t *a,
                                  const clip_regions_t *b);

/* Drop identity for a track whose clip has been DELETED.
 *
 * "Absent from the file" alone cannot mean deleted: a clip copied into an
 * empty slot is also absent until Move saves, and dropping identity there
 * would break the case where the user copies a clip and launches it. The
 * distinction is HISTORY -- a clip that existed in the previous parse and is
 * gone from this one was deleted; one that never existed may simply be new.
 *
 * Call with the regions as they were BEFORE the re-parse. */
void clip_regions_forget_deleted(const clip_regions_t *before,
                                 const clip_regions_t *after,
                                 clip_state_t *st);

#ifdef __cplusplus
}
#endif
#endif /* CLIP_REGIONS_H */
