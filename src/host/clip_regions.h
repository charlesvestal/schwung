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
 * THE ONE EXCEPTION IS AN EVENT, NOT A WEAKENING: a transport START launches
 * each track's SELECTED clip in EVERY view, and the file is the only thing
 * that knows which clip that is. So a track whose identity says "nothing
 * playing" AND for which a Start is pending is seeded too. See the long note
 * in clip_regions_seed_state() -- the user hit this by building a clip in the
 * step editor and pressing Play, where Note view's pad gate means no LED can
 * ever supply identity.
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
    double loop_start;   /* quarter notes */
    double loop_len;     /* quarter notes; 0 if unknown */
    /* The clip's OWN time signature, 0/0 when the file did not carry one
     * (older firmware, or a clip written before the feature). Move stores one
     * per clip AND one song-wide; ask through
     * clip_regions_quarters_per_bar(), which falls back in that order.
     *
     * Nothing about a LANE needs this: every number in Song.abl is in quarter
     * notes, and changing a set to 11/8 changed not one of them (measured
     * 2026-09-12). It is here for the one conversion that does need it --
     * turning the step editor's BAR COUNT into a length. */
    int    sig_upper;
    int    sig_lower;
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
    /* The step editor's grid, VERBATIM ("1/16", "1/8t", ...). Carried as text
     * as well as parsed because an unrecognised form must be visible rather
     * than silently turned into a number -- see step_resolution. */
    char   step_res_raw[12];
    /* Is the grid a TRIPLET one ("1/16t")? Not derivable from
     * `step_resolution` -- 1/16t and a hypothetical 1/24 have the same
     * duration -- and it changes which BUTTONS are steps at all: Move lays a
     * triplet grid out three-to-a-group and DEACTIVATES every fourth button,
     * so a page is 12 steps rather than 16. Measured 2026-09-13: one
     * right-arrow at 1/16t moved the scroll 0 -> 2.0, which is exactly 12
     * steps of 1/6 quarter. */
    int    step_grid_triplet;
    /* The SONG's time signature, 0/0 if absent. Load-bearing for a clip Move
     * has not saved yet: the clip is not in the file, but the song is. */
    int    sig_upper;
    int    sig_lower;

} clip_regions_t;

/* Parse a Song.abl. Returns 1 on success. On failure `out->valid` is 0 and
 * the caller must treat every field as unknown -- a partial parse is not a
 * usable answer. */
int clip_regions_parse_file(const char *path, clip_regions_t *out);

/* Same, over a buffer already in memory (what the tests drive). */
int clip_regions_parse(const char *json, size_t len, clip_regions_t *out);

/* Seed identity for tracks we have nothing for. Never touches a track with a
 * live observed clip, and never sets an anchor: the file says what is
 * selected, not when it started.
 *
 * "Nothing for" is either no identity at all, or identity saying nothing is
 * playing WITH a transport Start pending -- a Start launches the selected
 * clip. The anchor for that case is left to clip_state_anchor_pending(),
 * which the caller must run straight afterwards (shim_worker.c does). */
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
/* Quarter notes per bar for one clip: its own signature, else the song's,
 * else 4/4. `upper * 4 / lower` -- an 11/8 bar is 11 eighths = 5.5 quarters,
 * so a 12-quarter loop is ~2.18 bars and NOT 3. Anything that multiplies a
 * bar count by 4 is wrong twice over: the beats per bar AND the beat's unit.
 *
 * Pure, and safe with a NULL or an out-of-range position: 4.0.
 *
 * The ONLY thing that needs this is turning the step editor's bar count into
 * a length -- every number in Song.abl is already in quarters. */
double clip_regions_quarters_per_bar(const clip_regions_t *rg,
                                     int track, int slot);

/* IS `dst` A DUPLICATE OF `src`? -- the test behind "a clip was copied".
 *
 * Move's Copy duplicates the selected clip into the next free slot, and the
 * file is the only place we see it, so a copy is recognised as "a slot that
 * was empty now holds a clip matching a sibling". The comparison is the
 * clip's CONTENT: how many notes, which note is lowest, and how long the
 * loop is.
 *
 * A NOTE-LESS CLIP IS NEVER A DUPLICATE, and that guard is load-bearing
 * rather than tidy. Two clips with no notes are indistinguishable by this
 * test, so without it any newly arrived empty clip would match any other and
 * have somebody's automation copied onto it. Move 2.1.0's Bounce Clips to
 * Audio lands exactly that way -- a new slot holding a clip with no notes --
 * and so does Shift+Step 14 before you play anything into it.
 *
 * Pure so it can be tested: the caller owns the "was empty, now exists" half,
 * which is the part that needs two parses to see. */
static inline int clip_region_is_duplicate_of(const clip_region_t *src,
                                              const clip_region_t *dst)
{
    if (!src || !dst || !src->exists || !dst->exists) return 0;
    if (dst->note_count == 0 && dst->first_note < 0) return 0;
    if (src->note_count != dst->note_count) return 0;
    if (src->first_note != dst->first_note) return 0;
    if (src->loop_len != dst->loop_len) return 0;
    return 1;
}

/* THE SELECTED CLIP on `track`, from the file's `isPlaying` flag, or -1.
 *
 * NOT the same question as "what is playing", and conflating them is what
 * made p-locks refuse on a stopped track. The live identity in clip_state
 * answers PLAYBACK -- it is decoded from the pad LEDs and correctly says
 * clip_slot -1 when a track is playing nothing. But Move's step editor still
 * shows a clip, and a p-lock is an EDIT of that clip: it has to work with the
 * transport stopped, which is how most step editing is done.
 *
 * Move records the selection as `isPlaying` on the clip, which is its name
 * for it and not ours -- it survives a stop, and after Shift+Step 14 it names
 * the clip just created. Measured 2026-09-13: transport stopped, editor on
 * T3s2, the live identity said "nothing playing" and the file said
 * isPlaying on exactly T3s2.
 *
 * It is the FILE, so it is as old as Move's last save. Prefer the live
 * identity while something is actually playing; this is for when nothing is. */
static inline int clip_regions_selected_slot(const clip_regions_t *rg, int track)
{
    if (!rg || !rg->valid || track < 0 || track >= CLIP_TRACKS) return -1;
    for (int s = 0; s < CLIP_SLOTS; s++)
        if (rg->slots[track][s].exists && rg->slots[track][s].is_playing)
            return s;
    return -1;
}

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
