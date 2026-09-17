#include "lane_store.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

void lane_store_reset(lane_store_t *st) {
    if (st) memset(st, 0, sizeof(*st));
}

/* A key that does not fit lane_t's storage is REFUSED, not truncated.
 * Truncating would let two different over-length keys collide onto the
 * same stored (short) string -- binding a lane to the wrong parameter,
 * which this whole design exists to prevent -- and would silently orphan
 * whatever a previous truncated write already put there. `target`/`param`
 * mirror lane_t's field widths exactly; lane_alloc must agree with this or
 * it could allocate a lane that lane_find can never find again. */
static int lane_key_fits(const char *target, const char *param) {
    return strlen(target) < sizeof(((lane_t *)0)->target) &&
           strlen(param)  < sizeof(((lane_t *)0)->param);
}

lane_t *lane_find(lane_store_t *st, const char *target, const char *param,
                  int track, int slot) {
    if (!st || !target || !param) return 0;
    if (!lane_key_fits(target, param)) return 0;
    for (int i = 0; i < LANE_MAX; i++) {
        lane_t *ln = &st->lanes[i];
        if (!ln->used) continue;
        /* THE POSITION IS TESTED FIRST, and it is half the key. Comparing
         * only (target, param) gave one lane per parameter across all 8 clip
         * slots: recording the same knob against a second clip returned the
         * FIRST clip's lane, so the take landed in it, overflowed it and
         * interleaved with the other clip's curve. Two cheap integer
         * comparisons, in front of the two strcmps, is the whole fix. */
        if (ln->track != track || ln->slot != slot) continue;
        if (strcmp(ln->target, target) == 0 && strcmp(ln->param, param) == 0)
            return ln;
    }
    return 0;
}

lane_t *lane_alloc(lane_store_t *st, const char *target, const char *param,
                   int track, int slot, const lane_fingerprint_t *fp) {
    if (!st || !target || !param) return 0;
    if (!lane_key_fits(target, param)) return 0;
    lane_t *ln = lane_find(st, target, param, track, slot);
    if (ln) return ln;
    for (int i = 0; i < LANE_MAX; i++) {
        ln = &st->lanes[i];
        if (ln->used) continue;
        memset(ln, 0, sizeof(*ln));
        ln->used = 1;
        snprintf(ln->target, sizeof(ln->target), "%s", target);
        snprintf(ln->param, sizeof(ln->param), "%s", param);
        ln->track = track;
        ln->slot = slot;
        if (fp) ln->fp = *fp;
        return ln;
    }

    /* FULL -- SO TAKE AN ORPHAN'S SLOT BEFORE REFUSING.
     *
     * An orphaned lane belongs to a clip that has been DELETED. It is kept on
     * purpose: deleting a clip is undoable, and the lane coming back with it
     * is the whole reason orphaning is a flag rather than a free(). But it is
     * kept INVISIBLY, and it goes on occupying one of LANE_MAX -- so a user
     * who deletes clips for a while arrives at a slot that silently refuses
     * new automation, with nothing on screen to say why or what to clear.
     * Refusing because of clips that no longer exist is the worse failure.
     *
     * NEVER EVICT ONE THAT IS STILL DRIVING. Orphaning does not release the
     * modulation override (chain_set_clip_deleted only sets the flag), and
     * this function is pure -- it cannot hand an override back. Dropping a
     * driving lane would pin its parameter wherever the automation last wrote
     * it with nothing left to move it, which is the same hazard that keeps
     * the clear verbs in the chain rather than here.
     *
     * Oldest-first is not worth the field: any orphan is a clip the user
     * deleted, so the first one found is as good a victim as the best one. */
    for (int i = 0; i < LANE_MAX; i++) {
        ln = &st->lanes[i];
        if (!ln->used || !ln->orphaned || ln->driving) continue;
        memset(ln, 0, sizeof(*ln));
        ln->used = 1;
        snprintf(ln->target, sizeof(ln->target), "%s", target);
        snprintf(ln->param, sizeof(ln->param), "%s", param);
        ln->track = track;
        ln->slot = slot;
        if (fp) ln->fp = *fp;
        ln->evicted_orphan = 1;
        return ln;
    }
    return 0;   /* full of LIVE lanes: the caller reports it, never silently discards */
}

static int lane_nearest(const lane_t *ln, double phase) {
    int best = 0;
    double bestd = 1e30;
    for (int i = 0; i < ln->n; i++) {
        double d = ln->pts[i].phase - phase;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

void lane_write(lane_t *ln, double phase, float value, int hold) {
    lane_write_span(ln, phase, value, hold, 0.0);
}

/* See lane_point_t: `span` is how long a HELD point stands before the lane
 * goes back to whatever is underneath. 0 keeps the old meaning (until the next
 * point), which is what every recorded sweep and every lane already on disk
 * means. */
void lane_write_span(lane_t *ln, double phase, float value, int hold, double span) {
    /* `phase < 0.0` is false for NaN, so a NaN phase would otherwise sail
     * through every comparison below (insertion, thinning, overflow-nearest)
     * and land in pts[] -- isfinite() is the only comparison NaN cannot
     * spoof. A non-finite value is refused for the same reason: it would be
     * stored verbatim and handed straight to a synth parameter. */
    if (!ln || !ln->used || !isfinite(phase) || phase < 0.0 || !isfinite(value))
        return;

    /* Inside the window of an existing point: replace it. This is the thinning
     * rule AND the second-pass replace rule -- recording over a region
     * overwrites the points you pass rather than layering a second curve. */
    for (int i = 0; i < ln->n; i++) {
        double d = ln->pts[i].phase - phase;
        if (d < 0) d = -d;
        /* The SHAPE is replaced with the value, not left behind: recording a
         * sweep over an old p-lock must produce a slope, and p-locking over a
         * recorded point must produce a rectangle. Keeping the old flag here
         * made "write a ramp point where a held one was" a no-op in one
         * respect and a change in the other -- caught by the first test that
         * did it. */
        if (d < LANE_MIN_POINT_BEATS) {
            ln->pts[i].value = value;
            ln->pts[i].hold = hold ? 1 : 0;
            /* The SPAN is replaced with the shape, for the reason the flag is:
             * recording a sweep over a lock must leave a slope with no span,
             * and locking over a recorded point must leave a step-long
             * rectangle. */
            ln->pts[i].span = (float)((span > 0.0 && isfinite(span)) ? span : 0.0);
            return;
        }
    }

    if (ln->n >= LANE_POINTS_MAX) {
        /* Degrade resolution rather than drop the gesture -- a lost write in
         * the middle of a sweep is a hole the user cannot see or fix.
         * Deliberately does NOT re-check LANE_MIN_POINT_BEATS against the
         * new neighbours: that spacing is what everywhere else prevents,
         * but a full lane replacing its nearest point can legitimately land
         * closer than the minimum. Local to this branch, not a global
         * relaxation of the rule. */
        int i = lane_nearest(ln, phase);
        ln->pts[i].phase = phase;
        ln->pts[i].value = value;
        ln->pts[i].hold = hold ? 1 : 0;
        ln->pts[i].span = (float)((span > 0.0 && isfinite(span)) ? span : 0.0);
        ln->full_hits++;
        /* Re-sort the single moved element. */
        while (i > 0 && ln->pts[i - 1].phase > ln->pts[i].phase) {
            lane_point_t t = ln->pts[i - 1];
            ln->pts[i - 1] = ln->pts[i]; ln->pts[i] = t; i--;
        }
        while (i + 1 < ln->n && ln->pts[i + 1].phase < ln->pts[i].phase) {
            lane_point_t t = ln->pts[i + 1];
            ln->pts[i + 1] = ln->pts[i]; ln->pts[i] = t; i++;
        }
        return;
    }

    int at = ln->n;
    for (int i = 0; i < ln->n; i++) {
        if (ln->pts[i].phase > phase) { at = i; break; }
    }
    for (int i = ln->n; i > at; i--) ln->pts[i] = ln->pts[i - 1];
    ln->pts[at].phase = phase;
    ln->pts[at].value = value;
    ln->pts[at].hold = hold ? 1 : 0;
    ln->pts[at].span = (float)((span > 0.0 && isfinite(span)) ? span : 0.0);
    ln->n++;
}

int lane_eval(const lane_t *ln, double phase, double loop_start,
              double loop_len, int stepped,
              float *out) {
    if (!ln || !ln->used || !out || ln->n <= 0) return 0;
    if (ln->stale || ln->orphaned) return 0;
    if (loop_len <= 0.0 || !isfinite(phase) || !isfinite(loop_start)) return 0;
    const double win_hi = loop_start + loop_len;

    /* Only points that are INSIDE the clip as it is right now. `>=` is false
     * for a NaN phase, which is exactly how a NaN write used to pass this
     * gate and get counted as in-range -- guard independently of
     * lane_write's own check, since the writer and this reader are called
     * by different tasks' code and a corrupt point must not become a value
     * just because it is skipped rather than trusted. A non-finite stored
     * point is treated as absent (skipped), not as ending the scan, so a
     * single corrupt entry cannot hide every valid point behind it. */
    int first = -1, last = -1;
    for (int i = 0; i < ln->n; i++) {
        if (!isfinite(ln->pts[i].phase)) continue;
        /* The loop is a WINDOW in clip time, so a point can be dormant at
         * EITHER end -- below the loop start as well as past its end. A
         * prefix test ([0, loop_len)) was correct only while phases were
         * loop-relative, and with clip-relative storage it would play the
         * material before a bars-3-to-5 loop as if it were inside it. */
        if (ln->pts[i].phase < loop_start) continue;
        if (ln->pts[i].phase >= win_hi) break;
        if (first < 0) first = i;
        last = i;
    }
    if (last < 0) return 0;

    /*
     * A SPANNED POINT IS AN EDIT TO ONE STEP, so it owns [phase, phase+span)
     * and NOTHING ELSE. Asked first, because inside that window it beats every
     * rule below -- including the "before the first point" and "after the last
     * point" holds, which are what made a single lock mean the whole bar and
     * the bar BEFORE it too.
     *
     * Outside every span the lane answers as if the spanned points were not
     * there: that is what lets a recorded sweep keep playing underneath a
     * lock, and what makes a lane of nothing but locks go SILENT between them
     * so the knob owns the parameter again.
     */
    int have_span = 0;
    for (int i = first; i <= last; i++) {
        const lane_point_t *p = &ln->pts[i];
        if (!(p->hold && p->span > 0.0f && isfinite(p->span))) continue;
        have_span = 1;
        if (phase >= p->phase && phase < p->phase + (double)p->span) {
            *out = p->value;
            return 1;
        }
    }
    if (have_span) {
        /* Re-run the window over the UNSPANNED points only. A lane that is
         * nothing but locks has none, and answers "nothing to say" between
         * them -- which releases the override, which is the whole point of a
         * lock ending at its step. */
        int f2 = -1, l2 = -1;
        for (int i = first; i <= last; i++) {
            const lane_point_t *p = &ln->pts[i];
            if (p->hold && p->span > 0.0f && isfinite(p->span)) continue;
            if (f2 < 0) f2 = i;
            l2 = i;
        }
        if (l2 < 0) return 0;
        first = f2; last = l2;
    }

    if (phase <= ln->pts[first].phase) { *out = ln->pts[first].value; return 1; }
    if (phase >= ln->pts[last].phase) { *out = ln->pts[last].value; return 1; }

    for (int i = first; i < last; i++) {
        const lane_point_t *a = &ln->pts[i];
        const lane_point_t *b = &ln->pts[i + 1];
        /* A non-finite point mid-span is skipped, never used as an
         * interpolation endpoint -- letting it through computed span=NaN,
         * t=NaN, and an affirmative-looking NaN result. */
        if (!isfinite(a->phase) || !isfinite(b->phase)) continue;
        /* A spanned lock is not an endpoint for anything: it owns its own
         * window and is invisible everywhere else, so a sweep either side of
         * it interpolates across as though it were not in the array. */
        if (a->hold && a->span > 0.0f) continue;
        if (b->hold && b->span > 0.0f) {
            /* ...and the segment ENDING on one runs to the next unspanned
             * point instead. Walking forward keeps the array order. */
            int k = i + 2;
            while (k <= last && ln->pts[k].hold && ln->pts[k].span > 0.0f) k++;
            if (k > last) { if (phase >= a->phase) { *out = a->value; return 1; } continue; }
            b = &ln->pts[k];
            if (phase > b->phase) continue;
        }
        if (phase < a->phase || phase > b->phase) continue;
        /* `stepped` is the PARAMETER's type (an enum cannot ramp); `a->hold`
         * is this POINT's own shape. Either one holds, and the point's flag is
         * what makes a p-lock sound like a step rather than a glide into the
         * next one.
         *
         * AT EXACTLY THE NEXT POINT'S PHASE, THE NEXT POINT WINS. A rectangle
         * runs up to the following point and stops there -- extending it
         * through that phase makes the new value one evaluation late, and at
         * the wrap of a doubled loop it is a whole block of the wrong value.
         * The interpolating branch below already does this (t = 1 gives
         * b->value); only the holding one had to be told. */
        if (stepped || a->hold) {
            *out = (phase >= b->phase) ? b->value : a->value;
            return 1;
        }
        double span = b->phase - a->phase;
        if (span <= 0.0) { *out = b->value; return 1; }
        double t = (phase - a->phase) / span;
        *out = (float)(a->value + t * (b->value - a->value));
        return 1;
    }
    *out = ln->pts[last].value;
    return 1;
}

int lane_fingerprint_matches(const lane_t *ln, const lane_fingerprint_t *now) {
    if (!ln || !now) return 0;
    /* NO FINGERPRINT RECORDED IS NOT A MATCH. {0, -1} is what a lane carries
     * when nothing ever told it the clip's content: every lane written before
     * the parser counted notes, and every lane written while the clip itself
     * was unknown. NEITHER loop field is compared (see below), so with the
     * content half at its absent values the test would degenerate to nothing
     * at all -- such a lane would bind to the FIRST clip it met and play.
     * Stale is the honest answer: silent, retained, re-recordable.
     *
     * It also makes a lane recorded against a genuinely note-free clip stale.
     * Deliberate: "empty" and "unknown" are the same bytes here, and of the
     * two readings only this one cannot be confidently wrong. */
    if (lane_fp_absent(&ln->fp)) return 0;
    /* NEITHER loop_start NOR loop_len is compared, for one reason: a clip
     * whose loop area the user edited is the same clip, and going stale on it
     * is SILENT -- the automation just stops, with no gesture that restores it
     * short of recording the pass again. Both live in the fingerprint for
     * diagnostics only, and the content half is what discriminates.
     *
     * This costs nothing in the origin: phases are stored LOOP-RELATIVE
     * (shadow_slot_clip_phase subtracts loop_start), and clip-relative storage
     * was considered and rejected -- loop_start is observable by nothing for a
     * clip just made and then edited (Song.abl is ~35 s stale, and the OLED bar
     * strip does not show where the loop begins), so re-origining would have to
     * guess, putting every value a bar out while looking healthy. */
    if (ln->fp.note_count != now->note_count) return 0;
    if (ln->fp.first_note != now->first_note) return 0;
    return 1;
}

/* ---- the recording pass ----------------------------------------------
 * NOT part of lane_write: the deserializer and any future editor write
 * through that, and neither swept anything. See lane_store.h.
 */

/* Delete every point in the phase span, in place. `lo` is EXCLUSIVE when
 * lo_open -- the previous write of this pass sits exactly there and must
 * survive -- and `hi` is always exclusive, because a point inside
 * LANE_MIN_POINT_BEATS of the incoming write is lane_write's to replace, not
 * this function's to drop.
 *
 * A non-finite stored phase compares false against both bounds and is KEPT.
 * lane_eval already skips it and lane_write already refuses to create one, so
 * compacting corruption away here would only hide it from the code that
 * reports it.
 *
 * RT: one pass over at most LANE_POINTS_MAX entries, no allocation. */
static void lane_erase_span(lane_t *ln, double lo, int lo_open, double hi) {
    if (!(hi > lo)) return;
    int w = 0;
    for (int r = 0; r < ln->n; r++) {
        const double ph = ln->pts[r].phase;
        const int above = lo_open ? (ph > lo) : (ph >= lo);
        if (isfinite(ph) && above && ph < hi) continue;   /* swept: drop it */
        if (w != r) ln->pts[w] = ln->pts[r];
        w++;
    }
    ln->n = w;
}

void lane_record_point(lane_t *ln, double phase, float value,
                       double loop_start, double loop_len, int hold) {
    if (!ln || !ln->used) return;
    /* The SAME validity lane_write demands, checked before the erase: a write
     * that is going to be refused must not erase anything on its way to being
     * refused, and must not move the pass's phase either. */
    if (!isfinite(phase) || phase < 0.0 || !isfinite(value)) return;

    if (ln->rec_active) {
        const double prev = ln->rec_last_phase;
        /* THE SHARED COMPUTATION. lane_tick's suppression asks the same
         * function what the pass's extent is, so the region that goes silent
         * and the region that gets erased cannot drift apart -- a disagreement
         * erases points the lane is still playing. A travel outside the
         * threshold (or -1.0, "cannot tell") erases nothing: two writes that
         * far apart are not one gesture, so the lane between them is not this
         * pass's to delete. That is also what bounds the damage -- the erased
         * span can never exceed LANE_PASS_GAP_BEATS, whichever branch ran. */
        const double travel = lane_pass_travel(prev, phase, loop_start, loop_len);
        if (travel >= 0.0 && travel <= LANE_PASS_GAP_BEATS) {
            if (phase >= prev) {
                lane_erase_span(ln, prev, 1, phase);
            } else {
                /* WRAPPED: the span the knob passed over is (prev, win_hi)
                 * then [loop_start, phase). The WINDOW's start is included --
                 * it is the wrap boundary itself, which the sweep crossed --
                 * and it is `loop_start`, not 0.0: with clip-relative phases a
                 * bars-3-to-5 loop wraps to beat 8, and erasing from 0 would
                 * delete the material BEFORE the loop, which this pass never
                 * touched and the user cannot see. */
                lane_erase_span(ln, prev, 1, loop_start + loop_len);
                lane_erase_span(ln, loop_start, 0, phase);
            }
        }
    }

    lane_write(ln, phase, value, hold);
    /* Only AFTER a write that was accepted, so the next write's swept span
     * starts where this one actually landed. */
    ln->rec_active = 1;
    ln->rec_last_phase = phase;
}

void lane_record_end(lane_t *ln) {
    if (!ln) return;
    ln->rec_active = 0;
    ln->rec_last_phase = 0.0;
}

double lane_pass_travel(double prev, double phase,
                        double loop_start, double loop_len) {
    /* isfinite() is the only test NaN cannot spoof: `prev < 0.0` is false for
     * NaN, so a NaN phase would otherwise arrive as a distance of NaN, which
     * compares false against both ends of the threshold and so reads as
     * "outside the pass" at one caller and could read as a usable number at
     * the next. -1.0 says it once, for everybody. */
    if (!isfinite(prev) || !isfinite(phase)) return -1.0;
    if (prev < 0.0 || phase < 0.0) return -1.0;

    /* THE WINDOW IS CHECKED BEFORE THE DIRECTION, not after. A `prev` outside
     * the current loop cannot have been swept from inside it -- the loop moved
     * under the pass -- and that is just as true walking FORWARD: prev 2.0 to
     * phase 8.2 on a loop of 8..20 reads as a tidy 6.2 beats of travel, and
     * only the gap threshold downstream stops it erasing across two bars the
     * gesture never touched. "Cannot tell" is the honest answer in both
     * directions, and it is one place rather than two. */
    if (!isfinite(loop_start) || loop_start < 0.0) return -1.0;
    if (!isfinite(loop_len) || loop_len <= 0.0) return -1.0;
    const double win_hi = loop_start + loop_len;
    if (prev < loop_start || prev >= win_hi) return -1.0;
    if (phase < loop_start || phase >= win_hi) return -1.0;

    if (phase >= prev) return phase - prev;
    /* Backwards, from inside the window: the pass crossed the window's wrap. */
    return (win_hi - prev) + (phase - loop_start);
}

int lane_pass_live_at(const lane_t *ln, double phase,
                      double loop_start, double loop_len) {
    if (!ln || !ln->used || !ln->rec_active) return 0;
    const double travel = lane_pass_travel(ln->rec_last_phase, phase,
                                           loop_start, loop_len);
    /* "Cannot tell" is NOT live. The lane driving is the ordinary state and
     * silence is the exception, so an unreadable phase must fall back to
     * playing rather than to a parameter that has quietly stopped following
     * its automation with nothing on screen to explain it. */
    return (travel >= 0.0 && travel <= LANE_PASS_GAP_BEATS) ? 1 : 0;
}

int lane_adopt_slot(lane_t *ln, int track, int slot,
                    double recorded_len, double now_len,
                    const lane_fingerprint_t *now_fp) {
    if (!ln || !ln->used || !ln->slot_pending) return 0;
    if (!lane_slot_is_pending(ln->slot)) return 0;   /* already keyed */
    if (ln->track != track) return 0;
    if (slot < 0) return 0;
    /* THE LENGTHS MUST AGREE, and this is the check that makes re-keying a
     * measurement rather than a guess. A clip deleted and remade inside the
     * ~10 s save window would otherwise hand the first take to the second
     * clip. Both must be real numbers: "unknown" is not a match.
     *
     * Compared with a tolerance because one side came off a pixel strip
     * (segments x quarters-per-bar) and the other out of a JSON float; a bar
     * is at least 2 quarters, so half a quarter cannot confuse two lengths
     * that differ by a bar. */
    if (!isfinite(recorded_len) || recorded_len <= 0.0) return 0;
    if (!isfinite(now_len) || now_len <= 0.0) return 0;
    /* A LENGTHENED CLIP IS STILL THE SAME CLIP.
     *
     * Equality alone refused Double Loop outright: the gesture doubles the
     * clip, so a take recorded against 4 quarters met a clip of 8 and the
     * lane stayed PENDING for good -- measured, `adopt=0 slot=-2`, and on the
     * device it presented as the locks on a just-doubled new clip never
     * playing.
     *
     * An integer MULTIPLE is accepted because that is what the lengthening
     * gestures produce (Double Loop doubles; adding bars repeats), and
     * because it barely widens the gate this check exists for: it stops a
     * clip deleted and REMADE inside the save window inheriting the take, and
     * a remade clip takes the DEFAULT length, which the old rule already
     * accepted as equal. The take's points sit in the first repeat either
     * way.
     *
     * Only LONGER. A clip shorter than the take is not this take's clip. */
    const double mult = now_len / recorded_len;
    const double near = mult - (double)(long)(mult + 0.5);
    if (now_len + 0.5 < recorded_len) return 0;
    if (fabs(recorded_len - now_len) > 0.5 && fabs(near) > 0.01) return 0;
    /* THE IDENTITY COMES WITH THE ROW, and only once the length has agreed.
     *
     * A gesture made blind has an ABSENT fingerprint -- there were no notes to
     * fingerprint -- and lane_fingerprint_matches refuses an absent one
     * outright, so without this the lane is re-keyed correctly and then goes
     * STALE the moment the clip appears: silent for good. Measured on
     * hardware exactly that way.
     *
     * INSIDE the length check on purpose. Binding first would let a clip that
     * is NOT ours leave its fingerprint on the lane, after which the lane is
     * no longer absent and the real clip could never bind. One gate, one
     * decision.
     *
     * NO RE-ORIGIN, which is what makes this different from
     * lane_adopt_fingerprint: a blind p-lock's phase came from the bar on
     * Move's own strip and is already true clip time, so moving it would take
     * the lock off the step that was pressed. */
    /* BOTH HALVES, OR NEITHER. `slot_pending` is the only licence this lane
     * has to take an identity, and clearing it while the fingerprint is still
     * absent shuts the door behind it forever: lane_tick's adopt-on-edit
     * branch requires a NON-absent fingerprint, and lane_adopt_fingerprint
     * requires `origin_pending`, which a p-lock never sets. The lane then
     * falls through to `stale = 1` on every tick and is retained and SILENT
     * for good -- writes land, nothing plays.
     *
     * Observed on hardware 2026-09-15: `synth:cr_decay` re-keyed to row 0 with
     * pend=0, stale=1, n growing as the user kept setting values that could
     * never be heard, while every lane beside it on another slot was fine.
     *
     * So a row without an identity is NOT adopted -- we stay pending and try
     * again next tick. In practice both come from the same parse and arrive
     * together; when they skew, waiting costs one tick and closing the latch
     * costs the lane. (A clip absent from Song.abl cannot supply a row either,
     * so this cannot wait forever on a note-free clip: no file entry, no row,
     * still pending, exactly as before.) */
    if (lane_fp_absent(&ln->fp)) {
        if (!now_fp || lane_fp_absent(now_fp)) return 0;
        ln->fp = *now_fp;
        ln->stale = 0;
        ln->adopted++;
    }
    ln->slot = slot;
    ln->slot_pending = 0;
    ln->pending_len = 0.0;
    return 1;
}

int lane_adopt_fingerprint(lane_t *ln, const lane_fingerprint_t *now) {
    if (!ln || !ln->used || !now) return 0;
    /* Only a lane that was never identified, and only one THIS SESSION
     * recorded blind -- see origin_pending in lane_store.h for why the second
     * test is not redundant. */
    if (!lane_fp_absent(&ln->fp)) return 0;
    if (!ln->origin_pending) return 0;
    /* And only a REAL fingerprint: adopting an absent one would be a no-op
     * that still cleared the pending state, stranding the take at origin 0. */
    if (lane_fp_absent(now)) return 0;
    /* The origin must be a usable number. A NaN or negative loop_start would
     * put every point somewhere unnameable, and the lane is still fixable as
     * it stands -- so refuse and wait for a better answer. */
    if (!isfinite(now->loop_start) || now->loop_start < 0.0) return 0;

    /* RE-ORIGIN, then adopt. The take was recorded against an assumed origin
     * of 0 (the blind window has no loop.start), so clip time is the recorded
     * phase plus the real loop_start. Points stay sorted: one constant added
     * to every phase preserves order. A non-finite stored phase is left alone
     * -- lane_eval already skips it, and moving it would invent a position
     * for a point that has none. */
    if (now->loop_start > 0.0) {
        for (int i = 0; i < ln->n; i++) {
            if (!isfinite(ln->pts[i].phase)) continue;
            ln->pts[i].phase += now->loop_start;
        }
        /* The recording pass's own mark moves with them, or the next write of
         * a take still in progress erases from the wrong place. */
        if (ln->rec_active && isfinite(ln->rec_last_phase))
            ln->rec_last_phase += now->loop_start;
    }
    ln->fp = *now;
    ln->origin_pending = 0;
    /* A lane that was stale only because it could not be identified is not
     * stale any more -- it has just been identified. */
    ln->stale = 0;
    ln->adopted++;
    return 1;
}

int lane_double(lane_t *ln, double loop_start, double loop_len) {
    if (!ln || !ln->used) return 0;
    if (!isfinite(loop_start) || loop_start < 0.0) return 0;
    if (!isfinite(loop_len) || loop_len <= 0.0) return 0;
    const double win_hi = loop_start + loop_len;

    /* Collected FIRST, then written: lane_write inserts in phase order and
     * shifts the array, so walking and writing in one pass would re-read
     * points this call had just added and double them again, forever. */
    lane_point_t src[LANE_POINTS_MAX];
    int n = 0;
    for (int i = 0; i < ln->n && n < LANE_POINTS_MAX; i++) {
        const double ph = ln->pts[i].phase;
        if (!isfinite(ph) || ph < loop_start || ph >= win_hi) continue;
        src[n++] = ln->pts[i];
    }

    int copied = 0;
    for (int i = 0; i < n; i++) {
        if (ln->n >= LANE_POINTS_MAX) break;   /* as much as fits, in order */
        /* SPAN CARRIED. The 4-argument form leaves `span` 0, which MEANS
         * "hold until the next point" -- so the doubled half's p-locks
         * widened from one step to the rest of the bar while the original
         * half stayed correct, and the two halves of a doubled loop stopped
         * sounding the same. That is the whole promise of Double Loop. */
        lane_write_span(ln, src[i].phase + loop_len, src[i].value,
                        src[i].hold, src[i].span);
        copied++;
    }
    return copied;
}

void lane_store_swap(lane_store_t *a, lane_store_t *b)
{
    if (!a || !b) return;
    /* Three memcpys through a static rather than a stack temporary: a
     * lane_store_t is 37 KB and this runs on the SPI callback, whose frame is
     * already the tightest budget in the system (patch_info_t took it to
     * 232 KB when SLOT_BUSES went 4 -> 8). Not reentrant, and does not need to
     * be: every caller is that one thread. */
    static lane_store_t tmp;
    memcpy(&tmp, a, sizeof(tmp));
    memcpy(a, b, sizeof(*a));
    memcpy(b, &tmp, sizeof(*b));
}
