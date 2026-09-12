#include "lane_store.h"
#include <stdio.h>
#include <string.h>

void lane_store_reset(lane_store_t *st) {
    if (st) memset(st, 0, sizeof(*st));
}

lane_t *lane_find(lane_store_t *st, const char *target, const char *param) {
    if (!st || !target || !param) return 0;
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
    if (!ln || !ln->used || phase < 0.0) return;

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
         * the middle of a sweep is a hole the user cannot see or fix. */
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
    if (loop_len <= 0.0) return 0;

    /* Only points that are INSIDE the clip as it is right now. */
    int last = -1;
    for (int i = 0; i < ln->n; i++) {
        if (ln->pts[i].phase >= loop_len) break;
        last = i;
    }
    if (last < 0) return 0;

    if (phase <= ln->pts[0].phase) { *out = ln->pts[0].value; return 1; }
    if (phase >= ln->pts[last].phase) { *out = ln->pts[last].value; return 1; }

    for (int i = 0; i < last; i++) {
        const lane_point_t *a = &ln->pts[i];
        const lane_point_t *b = &ln->pts[i + 1];
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
