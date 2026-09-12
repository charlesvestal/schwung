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

/* THINNING ONLY. Two writes closer together than this collapse into one:
 * ~5 ms at 120 BPM, below a knob detent's spacing and above the jitter of
 * sampling phase on the callback.
 *
 * IT IS NOT THE SECOND-PASS RULE, and this comment once claimed it was. A
 * 5 ms window CANNOT be: a second pass's writes land tens of milliseconds
 * from the first pass's, so they missed the window and the two curves
 * INTERLEAVED (20 -> 90 -> 40 -> 91 -> 60), which is audible jumping. Found
 * on hardware. Replacing a pass is a SWEPT REGION, not a point window --
 * lane_record_point below. */
#define LANE_MIN_POINT_BEATS 0.01

/* A recording pass erases the span it sweeps between consecutive writes, but
 * only while the writes keep coming. This bounds it: two writes further apart
 * in phase than this are not one gesture, so the lane between them is not the
 * pass's to erase.
 *
 * One beat -- 500 ms at 120 BPM. A knob detent stream is tens of milliseconds
 * apart, so even a deliberately slow sweep stays an order of magnitude inside
 * this; a whole beat with no write is a hand that stopped. Measured in BEATS
 * rather than seconds because the lane is, so it scales with tempo the way the
 * user's playing does.
 *
 * It is deliberately NOT LANE_MIN_POINT_BEATS: conflating the thinning window
 * with the replace rule is what shipped the interleaving defect. */
#define LANE_PASS_GAP_BEATS 1.0

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
    /* The RECORDING PASS, which is also runtime and also never serialized.
     * A pass has a beginning and an end: `rec_active` is 0 until the first
     * write of a take and is cleared when recording stops or the lane stops
     * being the one playing, so the first write of the NEXT take cannot
     * erase back to wherever the last one happened to stop. */
    int    rec_active;
    double rec_last_phase;
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
 * the THINNING rule, and nothing more than that.
 *
 * ERASES NOTHING, EVER. lane_serial's deserializer and any future lane editor
 * write through here, and neither has a recording pass to sweep with: a load
 * must reproduce its document verbatim. Recording goes through
 * lane_record_point instead. */
void lane_write(lane_t *ln, double phase, float value);

/* ONE WRITE OF A RECORDING PASS.
 *
 * lane_write, plus: everything already in the lane strictly between the
 * previous write of THIS pass and this one is deleted, so a second pass wipes
 * the old curve as it passes over rather than interleaving with it. The first
 * write of a pass erases nothing -- there is no swept span yet -- and a gap
 * wider than LANE_PASS_GAP_BEATS erases nothing either.
 *
 * `loop_len` is the clip's CURRENT loop length, needed only to recognise a
 * wrap (phase < the previous write's). A wrapped sweep is ONE continuous
 * gesture: the swept span is (prev, loop_len) plus [0, phase), and the gap is
 * measured the same way round, so the erased span is never wider than the
 * threshold in either case. A non-positive or non-finite loop_len means "we
 * cannot tell", and a backwards phase then erases nothing.
 *
 * RT: bounded compaction over at most LANE_POINTS_MAX entries. */
void lane_record_point(lane_t *ln, double phase, float value, double loop_len);

/* End the pass. The next lane_record_point is a first write again. */
void lane_record_end(lane_t *ln);

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
