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

lane_t *lane_find(lane_store_t *st, const char *target, const char *param) {
    if (!st || !target || !param) return 0;
    if (!lane_key_fits(target, param)) return 0;
    for (int i = 0; i < LANE_MAX; i++) {
        lane_t *ln = &st->lanes[i];
        if (!ln->used) continue;
        if (strcmp(ln->target, target) == 0 && strcmp(ln->param, param) == 0)
            return ln;
    }
    return 0;
}

lane_t *lane_alloc(lane_store_t *st, const char *target, const char *param,
                   int track, int slot, const lane_fingerprint_t *fp) {
    if (!st || !target || !param) return 0;
    if (!lane_key_fits(target, param)) return 0;
    lane_t *ln = lane_find(st, target, param);
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
