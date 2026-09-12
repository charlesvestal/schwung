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
    return 0;   /* full: the caller reports it, never silently discards */
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

void lane_write(lane_t *ln, double phase, float value) {
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
        if (d < LANE_MIN_POINT_BEATS) { ln->pts[i].value = value; return; }
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
    ln->n++;
}

int lane_eval(const lane_t *ln, double phase, double loop_len, int stepped,
              float *out) {
    if (!ln || !ln->used || !out || ln->n <= 0) return 0;
    if (ln->stale || ln->orphaned) return 0;
    if (loop_len <= 0.0 || !isfinite(phase)) return 0;

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
        if (ln->pts[i].phase >= loop_len) break;
        if (first < 0) first = i;
        last = i;
    }
    if (last < 0) return 0;

    if (phase <= ln->pts[first].phase) { *out = ln->pts[first].value; return 1; }
    if (phase >= ln->pts[last].phase) { *out = ln->pts[last].value; return 1; }

    for (int i = first; i < last; i++) {
        const lane_point_t *a = &ln->pts[i];
        const lane_point_t *b = &ln->pts[i + 1];
        /* A non-finite point mid-span is skipped, never used as an
         * interpolation endpoint -- letting it through computed span=NaN,
         * t=NaN, and an affirmative-looking NaN result. */
        if (!isfinite(a->phase) || !isfinite(b->phase)) continue;
        if (phase < a->phase || phase > b->phase) continue;
        if (stepped) { *out = a->value; return 1; }
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
     * was unknown. loop_len is not compared (see below), so with the content
     * half at its absent values the test below degenerates to `loop_start
     * within eps` -- and every clip whose loop starts at 0.0 then fingerprints
     * identically, so such a lane binds to the WRONG clip and plays. Stale is
     * the honest answer: silent, retained, re-recordable.
     *
     * It also makes a lane recorded against a genuinely note-free clip stale.
     * Deliberate: "empty" and "unknown" are the same bytes here, and of the
     * two readings only this one cannot be confidently wrong. */
    if (ln->fp.note_count == 0 && ln->fp.first_note == -1) return 0;
    const double eps = 1e-6;
    double ds = ln->fp.loop_start - now->loop_start;
    if (ds < 0) ds = -ds;
    /* loop_len is deliberately NOT compared: a clip that grew is the same
     * clip, and that is the routine case this whole design tolerates. The
     * length lives in the fingerprint for diagnostics only. */
    if (ds > eps) return 0;
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

void lane_record_point(lane_t *ln, double phase, float value, double loop_len) {
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
        const double travel = lane_pass_travel(prev, phase, loop_len);
        if (travel >= 0.0 && travel <= LANE_PASS_GAP_BEATS) {
            if (phase >= prev) {
                lane_erase_span(ln, prev, 1, phase);
            } else {
                /* WRAPPED: the span the knob passed over is (prev, loop_len)
                 * then [0, phase). Phase 0 is INCLUDED -- it is the wrap
                 * boundary itself, which the sweep crossed. */
                lane_erase_span(ln, prev, 1, loop_len);
                lane_erase_span(ln, 0.0, 0, phase);
            }
        }
    }

    lane_write(ln, phase, value);
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

double lane_pass_travel(double prev, double phase, double loop_len) {
    /* isfinite() is the only test NaN cannot spoof: `prev < 0.0` is false for
     * NaN, so a NaN phase would otherwise arrive as a distance of NaN, which
     * compares false against both ends of the threshold and so reads as
     * "outside the pass" at one caller and could read as a usable number at
     * the next. -1.0 says it once, for everybody. */
    if (!isfinite(prev) || !isfinite(phase)) return -1.0;
    if (prev < 0.0 || phase < 0.0) return -1.0;
    if (phase >= prev) return phase - prev;
    /* Backwards. Only a known loop length can turn that into a wrap, and a
     * `prev` at or past the end cannot have been swept from inside this loop
     * -- the clip was re-cut under the pass. Unexplainable, so: cannot tell. */
    if (!isfinite(loop_len) || loop_len <= 0.0 || prev >= loop_len) return -1.0;
    return (loop_len - prev) + phase;
}

int lane_pass_live_at(const lane_t *ln, double phase, double loop_len) {
    if (!ln || !ln->used || !ln->rec_active) return 0;
    const double travel = lane_pass_travel(ln->rec_last_phase, phase, loop_len);
    /* "Cannot tell" is NOT live. The lane driving is the ordinary state and
     * silence is the exception, so an unreadable phase must fall back to
     * playing rather than to a parameter that has quietly stopped following
     * its automation with nothing on screen to explain it. */
    return (travel >= 0.0 && travel <= LANE_PASS_GAP_BEATS) ? 1 : 0;
}
