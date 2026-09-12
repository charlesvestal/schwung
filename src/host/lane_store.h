/*
 * lane_store — clip-associated automation lanes: the pure part.
 *
 * A lane is (clip position) x (target, param). Its content is breakpoints in
 * BEATS FROM THE CLIP'S LOOP START. Steps are a quantized view of this; the
 * lane itself never knows about steps and never reads Move's note content.
 *
 * THREE RULES THAT ARE NOT OBVIOUS
 * --------------------------------
 * 1. A LANE HAS NO LENGTH OF ITS OWN. Clip length is mutable from Move's step
 *    editor, so points are stored UNBOUNDED and evaluation wraps at whatever
 *    loop_len the clip currently has. Extending a clip reveals what was
 *    recorded there; shrinking it makes the tail dormant. Nothing is rescaled
 *    -- stretching a lane turns a filter sweep into a different filter sweep,
 *    which is the musically wrong answer even though it looks tidy.
 *
 * 2. EVAL CONSIDERS ONLY POINTS BELOW loop_len, and holds at both ends rather
 *    than interpolating across the wrap. Otherwise a dormant point past the
 *    end bends the audible curve while appearing nowhere on screen.
 *
 * 3. MOVE'S CLIPS HAVE NO IDENTITY. A clip in Song.abl carries name (usually
 *    ""), color, region, grooveId, notes -- no id, no uuid. So a lane is bound
 *    to a POSITION plus a FINGERPRINT of what was there when it was recorded.
 *    A mismatch makes the lane STALE: retained, silent, never guessed at. A
 *    lane playing the wrong clip's automation is worse than no lane at all.
 */
#ifndef LANE_STORE_H
#define LANE_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#define LANE_MAX          16   /* lanes per chain slot */
#define LANE_POINTS_MAX   64   /* breakpoints per lane */

/* Two writes closer together than this collapse into one. ~5 ms at 120 BPM:
 * below a knob detent's spacing, above the jitter of sampling phase on the
 * callback. It is also what makes a second pass REPLACE rather than layer. */
#define LANE_MIN_POINT_BEATS 0.01

typedef struct { double phase; float value; } lane_point_t;

/* What the clip looked like when the lane was recorded. Cheap, and each field
 * discriminates something the others do not: geometry catches a re-cut clip,
 * the note count catches a copy of a same-length clip, the first note catches
 * a same-length same-density different clip. */
typedef struct {
    double loop_start;
    double loop_len;
    int    note_count;
    int    first_note;   /* noteNumber of the earliest note, or -1 */
} lane_fingerprint_t;

typedef struct {
    int  used;
    char target[16];     /* "synth", "fx3", "midi_fx1" -- chain component addr */
    char param[32];
    int  track;          /* Move track 0..3 */
    int  slot;           /* clip slot 0..7 */
    lane_fingerprint_t fp;
    int  stale;          /* fingerprint mismatch: retained, silent */
    int  orphaned;       /* the clip was deleted; retained, silent */
    int  n;
    int  full_hits;      /* writes that had to replace a neighbour */
    /* Runtime, not content. `driving` is "we currently hold an override on
     * this target", so losing the phase can RELEASE it exactly once instead of
     * leaving the parameter stuck where the clip stopped. The punch pair is an
     * unarmed knob turn taking over until the loop comes round -- without it,
     * under an absolute lane, turning a knob does nothing audible. */
    int    driving;
    int    punch_until_wrap;
    double punch_phase;
    lane_point_t pts[LANE_POINTS_MAX];
} lane_t;

typedef struct { lane_t lanes[LANE_MAX]; } lane_store_t;

void   lane_store_reset(lane_store_t *st);

/* Find the lane for (target, param), or NULL. */
lane_t *lane_find(lane_store_t *st, const char *target, const char *param);

/* Find, else take a free slot and bind it. NULL when the store is full. */
lane_t *lane_alloc(lane_store_t *st, const char *target, const char *param,
                   int track, int slot, const lane_fingerprint_t *fp);

/* Insert or replace a breakpoint. Keeps pts[] sorted by phase. A write within
 * LANE_MIN_POINT_BEATS of an existing point overwrites that point's value --
 * which is both the thinning rule and the second-pass replace rule. */
void lane_write(lane_t *ln, double phase, float value);

/* Value at `phase`, considering only points below loop_len.
 * `stepped` = 1 for int/enum params (hold), 0 for float (linear).
 * Returns 1 and writes *out, or 0 for "this lane has nothing to say" --
 * which is NOT 0.0, and the caller must not treat it as a value. */
int lane_eval(const lane_t *ln, double phase, double loop_len, int stepped,
              float *out);

/* Does this lane still describe the clip that is there now? */
int lane_fingerprint_matches(const lane_t *ln, const lane_fingerprint_t *now);

#ifdef __cplusplus
}
#endif
#endif /* LANE_STORE_H */
