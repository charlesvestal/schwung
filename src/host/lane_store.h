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

#include <string.h>
#include <stdint.h>   /* lane_point_t's hold flag */

#ifdef __cplusplus
extern "C" {
#endif

/* Lanes per chain slot. IT SPANS CLIPS x PARAMETERS, which is what 16 did
 * not: the key is (track, slot, target, param), so 8 clip slots with two
 * automated parameters each exhausted the store outright.
 *
 * 32 is 8 clip slots x 4 parameters. The memory is irrelevant -- a lane_t is
 * ~1.1 KB and lane_store_t sits on chain_instance_t (~8.8 MB), NOT inside
 * patch_info_t, which is a stack local on the SPI callback, so 16 -> 32 costs
 * 18 KB of heap per slot and not a byte of that frame.
 *
 * WHAT CAPS IT IS THE PARAM CONTRACT, NOT MEMORY. `lanes:state` is served as
 * one param value, and SHADOW_PARAM_VALUE_LEN is 131072; the worst-case
 * document is LANE_SERIAL_MAX_BYTES, which a _Static_assert in lane_serial.c
 * holds below that ceiling. At 32 it is ~104 KB with ~20% to spare, and ~40
 * lanes is the hard wall. Going past that needs the document chunked across
 * several reads, which is a separate piece of work -- so do not raise this
 * without reading that assert. */
#define LANE_MAX          32
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

/* THE CLIP ROW WE CANNOT READ YET.
 *
 * A lane is keyed by (track 0..3, clip row 0..7) -- Move's session grid, one
 * column per track. The row comes from a session pad LED (Session view only)
 * or from Song.abl, and a clip you have just made has NEITHER: it has never
 * played, so no LED, and Move writes the file ~10 s late. MEASURED on hardware
 * 2026-09-14: 8-12 s, ending the second the file lands.
 *
 * The row is the ONLY missing fact in that window -- the track, the clip's
 * length and the step's phase all come off Move's own bar strip -- and it is
 * pure bookkeeping: it does not decide where the automation goes or what it
 * plays, only which clip the lane sticks to afterwards. So a gesture made in
 * the window is recorded against THIS row and re-keyed when the file names the
 * real one, rather than refused. Refusing is what the user met as "no clip on
 * this track" while looking straight at one.
 *
 * Out of range on purpose: nothing can collide with a real 0..7, and every
 * `slot < 0` guard already in the code keeps treating it as "no clip". */
#define LANE_SLOT_PENDING (-2)

/* THE KEY SPACE, NAMED. Move's session grid is four tracks of eight clip
 * rows, which this file has always stated in prose ("track 0..3, clip row
 * 0..7") and nowhere in code -- so the DESERIALIZER checked neither and
 * assigned whatever the document said. `lane_track` is the chain SLOT index,
 * which is why the track bound is 4 and not Move's track count. */
#define LANE_TRACKS 4
#define LANE_ROWS   8

/* A KEY THAT CAN ADDRESS A REAL CLIP. Deliberately excludes the pending
 * placeholder: this is the test a document must pass, and a provisional take
 * is never written to one (lane_serial.c). Runtime code asks
 * lane_slot_usable() instead, which does accept it. */
static inline int lane_key_in_range(int track, int slot) {
    return track >= 0 && track < LANE_TRACKS && slot >= 0 && slot < LANE_ROWS;
}

static inline int lane_slot_is_pending(int slot) { return slot == LANE_SLOT_PENDING; }
/* A row that can key a lane: a real one, or the pending placeholder. */
static inline int lane_slot_usable(int slot) {
    return (slot >= 0) || lane_slot_is_pending(slot);
}

/*
 * WHICH ROW A LANE IS MATCHED AGAINST THIS TICK.
 *
 * `current` goes to -1 for "we cannot name the row", which is NOT the fact
 * "a different clip is playing" — and reading the two as one silenced lanes
 * mid-playback with nothing else launched and the clip still audible. Losing
 * the ANSWER is not a negative answer; the same rule the param channel's
 * null-vs-"" tri-state exists for.
 *
 * So an unknown row falls back to the last one we had an answer for — which
 * may be the PENDING placeholder, and that case is not an edge: a clip Move
 * has not written to Song.abl yet HAS no row, so its lane is keyed to the
 * placeholder, and leaving that track evaporates it.
 *
 * It is the LAST KNOWN row rather than "match anything": one slot can hold
 * lanes for several rows, and matching anything would drive them all into the
 * same parameter at once. A positively-known DIFFERENT row still releases,
 * and a stopped transport still releases everything through the phase guard.
 */
static inline int lane_effective_slot(int current, int last_known) {
    if (lane_slot_usable(current)) return current;
    return lane_slot_usable(last_known) ? last_known : current;
}

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


/* The largest per-tick phase delta accepted as a LOOKAHEAD (see lane_tick).
 * A block is ~0.006 quarters at 133 BPM; anything approaching a sixteenth is
 * a wrap or a re-anchor, not a block, and is refused rather than used as a
 * lead -- a bad lead would play the NEXT step's lock on this one. */
#define LANE_LOOKAHEAD_MAX_BEATS 0.05
/* A POINT'S PHASE IS BEATS FROM THE CLIP'S START, NOT FROM ITS LOOP.
 *
 * The loop is a WINDOW over the lane -- [loop_start, loop_start + loop_len) --
 * and every phase argument below is in the same clip time, so moving or
 * resizing that window moves which part of the lane plays and never what the
 * lane means.
 *
 * Loop-relative storage was the first design and it was wrong in two ways the
 * user named: a sweep recorded over the notes in bar 3 of a bars-3-to-5 loop
 * SLID two bars when the loop was opened out to the whole clip, and a step
 * p-lock ("bar 3, step 5") cannot be turned into a stored phase at all without
 * knowing where the loop begins. The counter-argument -- that loop_start is
 * unobservable for a clip Move has not saved yet -- is true only for the ~10 s
 * before the save (measured; the figure used to be quoted as 35 s), and it is
 * arithmetic rather than a guess: `loop_start` is on each lane's own header
 * line, so a v1 document is shifted into clip time exactly on load.
 *
 * Points OUTSIDE the window are dormant, not deleted -- the same rule as a
 * shrunk clip, generalised from a prefix to a window. */
/* A POINT CAN BE A RECTANGLE. `hold` says "this value stands until the next
 * point" rather than ramping into it -- which is what a step p-lock IS: you
 * set a value ON a step, not a slope towards the next one. Under linear
 * interpolation two neighbouring p-locks glide into each other, which sounds
 * like automation and not like a sequencer.
 *
 * FREE, and that is why it goes in now: {double, float} is 12 bytes padded to
 * 16, so the flag costs nothing and the alternative is migrating documents
 * later. A `uint8_t` rather than a bitfield so the serializer can print it. */
/* A BREAKPOINT, and `span` is what makes a p-lock mean one STEP.
 *
 * `hold` says this point's value stands instead of ramping into the next one.
 * `span` says HOW LONG it stands: a p-lock is an edit to ONE STEP, so it ends
 * at the end of that step and the parameter goes back to whatever it would
 * otherwise be doing -- the recorded curve underneath, or the knob.
 *
 * Without it a single lock meant the whole bar, and backwards as well: with
 * one lock at step 4 every one of the sixteen steps read the locked value, and
 * playback was already at it before phase 1. That is what an automation lane
 * does and not what "lock this step" means, and the help text promised the
 * second one.
 *
 * ZERO IS THE LEGACY MEANING -- hold until the next point -- so every lane
 * already on disk keeps behaving exactly as it did, and a recorded sweep
 * (hold = 0) never has a span at all. The step LENGTH comes from the host,
 * which is the only side that knows the grid: the chain is told, never asked
 * to work it out. */
typedef struct { double phase; float value; uint8_t hold; float span; } lane_point_t;

/* What the clip looked like when the lane was recorded. ONLY THE CONTENT HALF
 * IS COMPARED (lane_fingerprint_matches): the note count catches a copy of a
 * same-length clip, the first note catches a same-length same-density
 * different clip. The two loop fields are recorded for diagnostics only --
 * a clip that grew is the same clip, and so is one whose loop area was
 * dragged, and going stale on either is silent. */
typedef struct {
    double loop_start;
    double loop_len;
    int    note_count;
    int    first_note;   /* noteNumber of the earliest note, or -1 */
} lane_fingerprint_t;

/* "NO FINGERPRINT WAS RECORDED", and it is the SAME FACT as "this lane's origin
 * is not yet known" -- which is why neither needs a field of its own, and why
 * nothing has to be persisted for a take recorded blind to be fixable later.
 *
 * {note_count 0, first_note -1} is what a lane carries when the clip could not
 * be fingerprinted: Move writes a new clip to Song.abl about 10 s after it is
 * made, and inside that window there are no notes to hash and no `loop.start`
 * to anchor to. A take recorded there is stored against an ASSUMED origin of
 * 0, so the absent fingerprint marks exactly the lanes that may still need
 * re-origining. Note 0 is a real note number, so the -1 is load-bearing. */
static inline int lane_fp_absent(const lane_fingerprint_t *fp) {
    return fp && fp->note_count == 0 && fp->first_note == -1;
}

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
    /* This lane took an ORPHAN's slot because the store was full. Recorded
     * rather than done silently: it is the one allocation that destroys
     * something, and "my automation for a deleted clip disappeared" needs an
     * answer better than a shrug. */
    int  evicted_orphan;
    /* How many times this lane's points were SHIFTED by a real loop start on
     * adoption. Counted, not flagged: a take stored against an assumed origin
     * of 0 that is never shifted is silent for good, and "my locks on that
     * clip do nothing" needs an answer better than a shrug. 0 or 1 in
     * practice. */
    int  reorigined;
    /* How many times this lane has had a real fingerprint stamped on it (0 or
     * 1 in practice). Counted rather than flagged because "a take recorded
     * blind was re-origined" is the kind of thing that must be reportable: by
     * ear an adopted lane and a lane that silently stayed at origin 0 are the
     * same until the loop is moved, and by then nobody remembers. */
    int  adopted;
    /* Runtime, not content. `driving` is "we currently hold an override on
     * this target", so losing the phase can RELEASE it exactly once instead of
     * leaving the parameter stuck where the clip stopped. The punch pair is an
     * unarmed knob turn taking over until the loop comes round -- without it,
     * under an absolute lane, turning a knob does nothing audible. */
    int    driving;
    /* THIS SESSION'S BLIND TAKE, and deliberately NOT serialized.
     *
     * Set when a lane is created while the clip's geometry is provisional --
     * Move has not written the clip to Song.abl yet, so there are no notes to
     * fingerprint and no loop.start to anchor to. It is what lets
     * lane_adopt_fingerprint tell "the clip I recorded against, thirty
     * seconds ago, still playing" from "a lane I loaded from disk whose clip
     * was never identified". Those are the same bytes on disk and must not be
     * the same decision: adopting the second would bind a lane to whatever
     * clip later occupied its position and PLAY it -- confidently wrong, the
     * one outcome this design refuses.
     *
     * The cost is a reboot inside the ~10 s window: the take stays at its
     * assumed origin with no identity, goes stale, and is silent until
     * re-recorded. Silent and retained is the failure this design chooses
     * every other time it has to choose. */
    int    origin_pending;
    /* THERE IS NO `slot_pending` FIELD. "The clip row is provisional" is
     * `slot == LANE_SLOT_PENDING` -- ask lane_slot_is_pending().
     *
     * It existed as a second latch beside the row, set and cleared at three
     * sites, and the two could only ever disagree one way: a DESERIALIZED
     * lane, which carried the row from the file and could not carry a
     * runtime-only flag. That combination -- row PENDING, latch clear -- was
     * the un-re-keyable zombie, and lane_adopt_slot's guard required both, so
     * such a lane could never be re-keyed and never went stale either.
     *
     * The reader refuses an out-of-range key now (lane_key_in_range), so the
     * one state that made two fields necessary cannot arrive, and keeping
     * them was keeping a "cleared one, forgot the other" bug available for
     * free. `pending_len` stays: it is DATA, not a restatement of the row. */
    /* The clip LENGTH the blind take was recorded against, read off Move's bar
     * strip. Kept so lane_adopt_slot can refuse a clip that is not the one we
     * were editing; 0 means "never recorded blind". Runtime only. */
    double pending_len;
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

/* HOW BIG THIS IS ALLOWED TO GET, enforced rather than described.
 *
 * Two comments stated the size in prose and BOTH were wrong -- one said
 * 18 KB, the other 37 KB, against a measured 53.2 KB; the first predates the
 * LANE_MAX 16 -> 32 raise and the second predates something else. The number
 * matters because a lane_store_t must never land on the SPI callback's frame
 * (patch_info_t alone took that to 232 KB when SLOT_BUSES went 4 -> 8), and a
 * prose number that drifts is exactly how it would: the next person sizes a
 * temporary against 18 KB.
 *
 * So the constraint is a budget the build checks, not a figure to keep in
 * sync. Raising LANE_MAX or LANE_POINTS_MAX past it fails HERE, where the
 * decision is, with the reason attached. */
#define LANE_STORE_MAX_BYTES (64 * 1024)
_Static_assert(sizeof(lane_store_t) <= LANE_STORE_MAX_BYTES,
               "lane_store_t is over budget: it must not sit on the SPI "
               "callback's stack frame, and lane_store_swap keeps one as a "
               "static for that reason");

/* How many lanes a snapshot CANNOT hold — the ones lane_serial.c refuses to
 * write because their clip cannot yet be identified (a pending row, or a real
 * row with no fingerprint).
 *
 * It exists so the loss can be COUNTED. Take a snapshot inside Move's save
 * window and it cannot contain the take just made; put that snapshot back and
 * the live take is replaced by a document that never held it. Both halves were
 * silent, and every other partial restore in this codebase reports a number —
 * a restore that says nothing is indistinguishable from one that worked. */
static inline int lane_store_provisional_count(const lane_store_t *st) {
    if (!st) return 0;
    int n = 0;
    for (int i = 0; i < LANE_MAX; i++) {
        const lane_t *ln = &st->lanes[i];
        if (!ln->used) continue;
        if (lane_slot_is_pending(ln->slot) || lane_fp_absent(&ln->fp)) n++;
    }
    return n;
}


void   lane_store_reset(lane_store_t *st);

/* Does this lane belong to that clip? The key is (track, slot, target,
 * param), so a clip's automation is every lane sharing its first two fields.
 *
 * THE STORE CANNOT CLEAR ANYTHING BY ITSELF. A driving lane holds a modulation
 * override on the chain, and dropping the lane without handing that back
 * leaves the parameter pinned at whatever the automation last wrote, with
 * nothing left to move it. So the caller walks these, releases each, and then
 * calls lane_clear_one -- which is why this is a predicate and not a
 * clear_clip() that would have to know what a chain is. */
static inline int lane_is_for_clip(const lane_t *ln, int track, int slot)
{
    return ln && ln->used && ln->track == track && ln->slot == slot;
}

static inline int lane_is_for_param(const lane_t *ln, int track, int slot,
                                    const char *target, const char *param)
{
    if (!lane_is_for_clip(ln, track, slot)) return 0;
    if (!target || !param) return 0;
    return strcmp(ln->target, target) == 0 && strcmp(ln->param, param) == 0;
}

/* Forget one lane. Its override must already have been released. */
static inline void lane_clear_one(lane_t *ln)
{
    if (ln) memset(ln, 0, sizeof(*ln));
}

/* ONE-DEEP UNDO, and it is a SWAP rather than a copy-back.
 *
 * Swapping makes undo its own inverse, so the same key is redo -- which
 * matters more here than in a text editor: an automation mistake is heard
 * rather than seen, and "put it back, no, the other one" is the actual
 * gesture. The cost is one extra lane_store_t on the instance (37 KB), which
 * is nothing beside the 8 MB it already carries, and no allocation.
 *
 * RT: a memcpy on the SPI callback. Taken before DISCRETE edits and once at
 * the start of a recording pass -- never per recorded point, which would be
 * 37 KB per breakpoint of a sweep. */
void   lane_store_swap(lane_store_t *a, lane_store_t *b);

/* Find the lane for (track, slot, target, param), or NULL.
 *
 * THE CLIP POSITION IS PART OF THE KEY. It was not, and `track`/`slot` were
 * written once by lane_alloc and never consulted again -- so there was one
 * lane per parameter across all 8 clip slots, and recording the same
 * parameter against a second clip silently TOOK OVER the first clip's lane.
 * Found on hardware: the second pass hijacked clip 1's lane, overflowed it to
 * LANE_POINTS_MAX and interleaved its values into the first take's curve,
 * while lane_tick's position gate correctly refused to play a lane bound to
 * slot 0 during clip 2 -- so the symptom was "it didn't record".
 *
 * The argument order matches lane_alloc's first five deliberately, so the
 * forwarding between them cannot silently transpose a pair. */
lane_t *lane_find(lane_store_t *st, const char *target, const char *param,
                  int track, int slot);

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
/* `hold` = 1 writes a rectangle (see lane_point_t); 0 is an ordinary
 * breakpoint. A recorded knob sweep is 0 -- it IS a slope -- and a step p-lock
 * is 1. */
void lane_write(lane_t *ln, double phase, float value, int hold);

/* lane_write, plus the SPAN a held point covers (see lane_point_t). A span of
 * 0 is exactly lane_write's behaviour, which is why that signature is left
 * alone -- every existing caller means "until the next point". */
void lane_write_span(lane_t *ln, double phase, float value, int hold, double span);

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
void lane_record_point(lane_t *ln, double phase, float value,
                       double loop_start, double loop_len, int hold);

/* End the pass. The next lane_record_point is a first write again. */
void lane_record_end(lane_t *ln);

/* HOW FAR A PASS HAS TRAVELLED, from `prev` forward to `phase`, in beats.
 *
 * THE ONE PLACE THE PASS'S EXTENT IS COMPUTED. lane_record_point erases the
 * span a pass sweeps and lane_tick must go SILENT over exactly that span --
 * two readings of "the pass is here", and a second copy of this arithmetic is
 * free to disagree with the first, which erases a region the lane is still
 * playing. Pure and exported so tests/host can drive it directly.
 *
 * Playback phase only ever increases, so `phase` below `prev` means the clip
 * LOOPED: the travelled distance is (loop_len - prev) + phase, never
 * phase - prev, which is negative and describes the untouched middle of the
 * lane rather than the swept ends.
 *
 * Returns -1.0 for "cannot tell" -- a non-finite or negative input, or a
 * backwards phase with no usable loop_len. A real distance is never negative,
 * so the sentinel cannot collide with an answer, and every caller must treat
 * it as neither inside nor outside the pass. */
double lane_pass_travel(double prev, double phase,
                        double loop_start, double loop_len);

/* Is a recording pass LIVE at `phase`? 1 only while `rec_active` and the
 * transport is within LANE_PASS_GAP_BEATS forward of the pass's last write.
 *
 * A live pass is exactly where the lane must NOT drive its parameter: the lane
 * is absolute, so the old curve would be written over the knob every block and
 * the take would be inaudible while it was being made. Bounded to the window
 * rather than to `rec_active` so the REST of the loop keeps playing, which is
 * what makes this punch-in/punch-out rather than a recording mode. */
int lane_pass_live_at(const lane_t *ln, double phase,
                      double loop_start, double loop_len);

/* Value at `phase`, considering only points below loop_len.
 * `stepped` = 1 for int/enum params (hold), 0 for float (linear).
 * Returns 1 and writes *out, or 0 for "this lane has nothing to say" --
 * which is NOT 0.0, and the caller must not treat it as a value. */
int lane_eval(const lane_t *ln, double phase, double loop_start,
              double loop_len, int stepped,
              float *out);

/* Does this lane still describe the clip that is there now? */
int lane_fingerprint_matches(const lane_t *ln, const lane_fingerprint_t *now);

/* DOUBLE the lane: every point inside the window is copied one window-length
 * later, so the automation repeats exactly as the notes do.
 *
 * Move's own Double Loop (Shift+Step 15) is documented as doubling a loop
 * "including its notes and automation", so a lane that did not follow would
 * leave the second half silent while the notes played -- the automation and
 * the music would disagree from that moment on.
 *
 * Called BEFORE the clip's new length is known: Move writes the doubled loop
 * to Song.abl about 10 s later, and the copies land in the second half, which
 * is dormant until the window grows to include it. That ordering is why this
 * takes the CURRENT window rather than reading a length that has not arrived.
 *
 * Points outside the window are left alone -- they belong to material this
 * gesture did not touch. A lane that would overflow LANE_POINTS_MAX copies as
 * much as it can, in phase order, rather than refusing: a partially doubled
 * lane is audibly close, and refusing outright would leave the second half
 * silent, which is the outcome this exists to prevent.
 *
 * Returns the number of points copied. RT: SPI callback, one pass, no
 * allocation. */
int lane_double(lane_t *ln, double loop_start, double loop_len);

/* ADOPT a real fingerprint onto a lane recorded blind, and re-origin its
 * points in the same step.
 *
 * The clip has just appeared in Song.abl, so two unknowns are answered at
 * once: which clip this is (its notes) and where its loop starts. A take
 * recorded in the blind window assumed an origin of 0, so every point is
 * shifted by the real `loop_start` -- exact arithmetic with the number that
 * just arrived, not a guess.
 *
 * REFUSES unless the lane's fingerprint is absent AND `origin_pending` is set
 * -- i.e. this session recorded it blind. A lane that was already identified
 * must never be re-labelled, and a lane LOADED from disk with an absent
 * fingerprint must never be labelled at all: on disk those two are the same
 * bytes, and adopting the loaded one would bind a lane to whatever clip later
 * occupied its position. The caller owns the other half of the guard -- that
 * this is the clip playing at the lane's own position.
 *
 * Returns 1 if it adopted, 0 if it refused. Idempotent by construction: after
 * adopting, the fingerprint is no longer absent.
 *
 * RT: SPI callback. One pass over at most LANE_POINTS_MAX points. */
int lane_adopt_fingerprint(lane_t *ln, const lane_fingerprint_t *now);

/* THE ROW ARRIVED. Re-key a lane recorded against LANE_SLOT_PENDING to the row
 * Song.abl now names, or refuse.
 *
 * `recorded_len` is the clip length the gesture was recorded against (read off
 * Move's bar strip); `now_len` is the length of the clip that has just
 * appeared. THE LENGTHS MUST MATCH, and that check is the whole difference
 * between this and a guess: without it, deleting the clip and making another
 * one inside the save window would hand the first take to the second clip --
 * confidently wrong, which is the outcome this design refuses everywhere else.
 * A mismatch leaves the lane pending, where it is visible and silent.
 *
 * `now_fp` (optional) is the arriving clip's fingerprint. A lane keyed blind
 * has an ABSENT one -- there were no notes yet -- and lane_fingerprint_matches
 * refuses an absent fingerprint outright, so without taking it here the lane
 * is re-keyed correctly and then goes STALE the moment the clip appears.
 * Taken INSIDE the length check, so a clip that is not ours cannot leave its
 * identity behind.
 *
 * IT RE-ORIGINS, through the same lane_take_identity as
 * lane_adopt_fingerprint. This said the opposite for a while -- "WITHOUT
 * re-origining, because a blind p-lock's phase is already true clip time" --
 * which holds only when the clip's origin is 0, and that is precisely what a
 * clip Move has not written cannot tell us: chain_set_clip_phase hands the
 * write side 0, so the lock lands in 0-space. The belief was measured false
 * and fixed in the .c, and this half of the comment survived it; anyone
 * reading only the header re-learned the disproven version with the disproof
 * one file away.
 *
 * Returns 1 if the lane was re-keyed. A row that arrives WITHOUT a usable
 * identity is refused rather than half-taken -- see the .c for why closing
 * the pending latch early strands the lane silently forever. */
int lane_adopt_slot(lane_t *ln, int track, int slot,
                    double recorded_len, double now_len,
                    const lane_fingerprint_t *now_fp);


#ifdef __cplusplus
}
#endif
#endif /* LANE_STORE_H */
