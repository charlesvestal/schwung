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

#include <stdint.h>
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
    /* The CONTENT half of a lane's fingerprint. Geometry alone cannot tell a
     * copied clip from the original -- same loop, different notes -- and
     * binding a lane to the wrong clip is the confidently-wrong answer this
     * whole pair of projects exists to refuse.
     *
     * first_note is -1 when there are no notes. NOT 0: note 0 is a real note,
     * so a zeroed field is indistinguishable from a clip whose earliest note
     * is the lowest one. clip_regions_parse writes -1 into every slot before
     * it scans, so an UNPARSED slot cannot read as note 0 either. */
    int note_count;
    int first_note;   /* noteNumber of the earliest note, or -1 */
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
 * Call with the regions as they were BEFORE the re-parse.
 *
 * `deleted_mask` (optional; pass NULL to skip) reports every (track, slot)
 * pair that went away, bit `track * CLIP_SLOTS + slot`, ASSIGNED rather than
 * OR'd -- it describes this one re-parse and nothing earlier.
 *
 * It answers a WIDER question than the identity half above, which only ever
 * looks at the clip a track is playing: an automation lane is bound to a grid
 * POSITION, so a lane on any of the eight positions has to be told, not just
 * the live one. And this function is the only place that can tell a deletion
 * from a clip Move has not saved yet, which is why the lane side is told from
 * here rather than inferring it from a single parse. */
void clip_regions_forget_deleted(const clip_regions_t *before,
                                 const clip_regions_t *after,
                                 clip_state_t *st,
                                 uint32_t *deleted_mask);
/* Every clip slot must fit the mask, or a deletion in the last slots is
 * invisible with nothing to say so. */
_Static_assert(CLIP_TRACKS * CLIP_SLOTS <= 32,
               "deleted_mask is a uint32_t and cannot address every clip slot");

#ifdef __cplusplus
}
#endif
#endif /* CLIP_REGIONS_H */
