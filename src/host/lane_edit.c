/* lane_edit.c -- see lane_edit.h. */
#include "lane_edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int span_points(const lane_t *ln, double lo, double len, lane_point_t *out)
{
    int n = 0;
    for (int i = 0; i < ln->n && n < LANE_POINTS_MAX; i++)
        if (lane_point_in_span(ln->pts[i].phase, lo, len)) out[n++] = ln->pts[i];
    return n;
}

/* Remove the span's points, then insert `pts` (already in the span), keeping
 * the lane sorted by phase. Capacity is the lane's own: a point that does not
 * fit is the only thing that can be lost, and the paste was pre-checked. */
static void replace_span(lane_t *ln, double lo, double len, const lane_point_t *pts, int n)
{
    lane_point_t keep[LANE_POINTS_MAX];
    int k = 0;
    for (int i = 0; i < ln->n; i++)
        if (!lane_point_in_span(ln->pts[i].phase, lo, len)) keep[k++] = ln->pts[i];
    int out = 0, a = 0, b = 0;
    while ((a < k || b < n) && out < LANE_POINTS_MAX) {
        if (b >= n || (a < k && keep[a].phase < pts[b].phase)) ln->pts[out++] = keep[a++];
        else ln->pts[out++] = pts[b++];
    }
    ln->n = out;
}

int lane_paste_span(lane_store_t *st, int track, int slot, double src, double dst,
                    double len, lane_journal_entry_t *je, lane_scope_fn scope, void *scope_ctx)
{
    if (!st || !je || !(len > 0.0)) return 0;
    uint32_t id = je->id;
    memset(je, 0, sizeof *je);
    je->id = id;
    je->lo = dst;
    je->len = len;

    /* Plan first, touch nothing: count the lanes and check the fit. */
    int touched = 0;
    for (int i = 0; i < LANE_MAX; i++) {
        const lane_t *ln = &st->lanes[i];
        if (!lane_is_for_clip(ln, track, slot)) continue;
        if (scope && !scope(ln, scope_ctx)) continue;
        lane_point_t s[LANE_POINTS_MAX], d[LANE_POINTS_MAX];
        int ns = span_points(ln, src, len, s), nd = span_points(ln, dst, len, d);
        if (!ns && !nd) continue;
        if (ln->n - nd + ns > LANE_POINTS_MAX) return -1;
        touched++;
    }
    if (touched > LANE_JOURNAL_LANES) return -1;

    for (int i = 0; i < LANE_MAX; i++) {
        lane_t *ln = &st->lanes[i];
        if (!lane_is_for_clip(ln, track, slot)) continue;
        if (scope && !scope(ln, scope_ctx)) continue;
        lane_point_t s[LANE_POINTS_MAX];
        lane_span_rec_t *r = &je->rec[je->nrec];
        int ns = span_points(ln, src, len, s);
        int nd = span_points(ln, dst, len, r->before);
        if (!ns && !nd) continue;
        for (int k = 0; k < ns; k++) s[k].phase += dst - src;   /* snapshot taken: overlap-safe */
        memcpy(r->after, s, sizeof(lane_point_t) * (size_t)ns);
        r->nb = nd;
        r->na = ns;
        snprintf(r->target, sizeof r->target, "%s", ln->target);
        snprintf(r->param, sizeof r->param, "%s", ln->param);
        r->track = track;
        r->slot = slot;
        r->fp = ln->fp;
        replace_span(ln, dst, len, s, ns);
        je->nrec++;
    }
    return je->nrec;
}

int lane_journal_apply(lane_store_t *st, const lane_journal_entry_t *je, int to_after)
{
    if (!st || !je || !je->id) return 0;
    int done = 0;
    for (int k = 0; k < je->nrec; k++) {
        const lane_span_rec_t *r = &je->rec[k];
        lane_t *ln = lane_find(st, r->target, r->param, r->track, r->slot);
        const lane_point_t *pts = to_after ? r->after : r->before;
        const int n = to_after ? r->na : r->nb;
        if (!ln) {
            if (!n) continue;                    /* nothing to bring back */
            ln = lane_alloc(st, r->target, r->param, r->track, r->slot, &r->fp);
            if (!ln) continue;
        }
        replace_span(ln, je->lo, je->len, pts, n);
        /* A whole-lane record that leaves nothing is a lane the edit CREATED
         * being undone: free it, or undone takes leave empty lanes holding
         * store slots. A span record keeps its lane -- only part was
         * touched. */
        if (!ln->n && je->len >= LANE_EDIT_WHOLE_LEN) lane_clear_one(ln);
        done++;
    }
    return done;
}

static const lane_t *find_c(const lane_store_t *st, const lane_t *like)
{
    for (int i = 0; i < LANE_MAX; i++) {
        const lane_t *ln = &st->lanes[i];
        if (ln->used && ln->track == like->track && ln->slot == like->slot &&
            strcmp(ln->target, like->target) == 0 && strcmp(ln->param, like->param) == 0)
            return ln;
    }
    return NULL;
}

/* Same POINTS -- field by field, since the struct has padding. */
static int same_points(const lane_t *a, const lane_t *b)
{
    const int na = a ? a->n : 0, nb = b ? b->n : 0;
    if (na != nb) return 0;
    for (int i = 0; i < na; i++) {
        const lane_point_t *p = &a->pts[i], *q = &b->pts[i];
        if (p->phase != q->phase || p->value != q->value || p->hold != q->hold || p->span != q->span)
            return 0;
    }
    return 1;
}

static void rec_lane(lane_span_rec_t *r, const lane_t *key, const lane_t *b, const lane_t *a)
{
    memset(r, 0, sizeof *r);
    snprintf(r->target, sizeof r->target, "%s", key->target);
    snprintf(r->param, sizeof r->param, "%s", key->param);
    r->track = key->track;
    r->slot = key->slot;
    r->fp = key->fp;
    r->nb = b ? b->n : 0;
    r->na = a ? a->n : 0;
    if (r->nb) memcpy(r->before, b->pts, sizeof(lane_point_t) * (size_t)r->nb);
    if (r->na) memcpy(r->after, a->pts, sizeof(lane_point_t) * (size_t)r->na);
}

int lane_journal_changed(const lane_store_t *before, const lane_store_t *after)
{
    if (!before || !after) return 0;
    int changed = 0;
    for (int i = 0; i < LANE_MAX; i++) {
        const lane_t *b = &before->lanes[i];
        if (b->used && !same_points(b, find_c(after, b))) changed++;
        const lane_t *a = &after->lanes[i];
        if (a->used && !find_c(before, a) && a->n) changed++;      /* a new lane */
    }
    return changed > LANE_JOURNAL_LANES ? -1 : changed;
}

int lane_journal_diff(const lane_store_t *before, const lane_store_t *after, lane_journal_entry_t *je)
{
    if (!before || !after || !je) return 0;
    uint32_t id = je->id;
    memset(je, 0, sizeof *je);
    je->id = id;
    je->lo = LANE_EDIT_WHOLE_LO;
    je->len = LANE_EDIT_WHOLE_LEN;
    /* Count first, so an edit too big to journal is refused whole. */
    int changed = 0;
    for (int i = 0; i < LANE_MAX; i++) {
        const lane_t *b = &before->lanes[i];
        if (b->used && !same_points(b, find_c(after, b))) changed++;
        const lane_t *a = &after->lanes[i];
        if (a->used && !find_c(before, a) && a->n) changed++;      /* a new lane */
    }
    if (changed > LANE_JOURNAL_LANES) { je->id = 0; return -1; }
    for (int i = 0; i < LANE_MAX; i++) {
        const lane_t *b = &before->lanes[i];
        if (b->used) {
            const lane_t *a = find_c(after, b);
            if (!same_points(b, a)) rec_lane(&je->rec[je->nrec++], b, b, a);
        }
        const lane_t *a = &after->lanes[i];
        if (a->used && !find_c(before, a) && a->n) rec_lane(&je->rec[je->nrec++], a, NULL, a);
    }
    return je->nrec;
}

int lane_stash_row(lane_store_t *st, int track, int slot, lane_stash_t *sh)
{
    if (!st || !sh) return 0;
    uint32_t id = sh->id;
    memset(sh, 0, sizeof *sh);
    sh->id = id;
    for (int i = 0; i < LANE_MAX; i++) {
        lane_t *ln = &st->lanes[i];
        if (!lane_is_for_clip(ln, track, slot)) continue;
        sh->lanes[sh->n++] = *ln;
        lane_clear_one(ln);
    }
    return sh->n;
}

int lane_unstash_row(lane_store_t *st, lane_stash_t *sh, int track, int slot)
{
    if (!st || !sh) return 0;
    int done = 0;
    for (int k = 0; k < sh->n; k++) {
        lane_t src = sh->lanes[k];
        lane_t *ln = lane_find(st, src.target, src.param, track, slot);
        if (!ln) ln = lane_alloc(st, src.target, src.param, track, slot, &src.fp);
        if (!ln) continue;
        *ln = src;
        ln->track = track;
        ln->slot = slot;
        ln->driving = 0;             /* runtime state: the tick re-derives it */
        ln->orphaned = 0;
        ln->stale = 0;
        ln->rec_active = 0;
        ln->punch_until_wrap = 0;
        done++;
    }
    memset(sh, 0, sizeof *sh);
    return done;
}

int lane_voice_scope_init(lane_voice_scope_t *vs, const char *map, const char *notes)
{
    if (!vs) return 0;
    memset(vs, 0, sizeof *vs);
    if (!map || !*map || !notes || !*notes) return 0;
    int any = 0;
    for (const char *p = notes; *p;) {
        char *end;
        long note = strtol(p, &end, 10);
        if (end == p) break;
        any = 1;
        /* Find "<note>:" at the start of a segment. */
        for (const char *seg = map; seg && *seg && vs->n < LANE_VOICE_SCOPE_NOTES;) {
            char *colon;
            long n = strtol(seg, &colon, 10);
            const char *next = strchr(seg, ';');
            if (colon != seg && *colon == ':' && n == note) {
                vs->lo[vs->n] = colon + 1;
                vs->hi[vs->n] = next ? next : colon + 1 + strlen(colon + 1);
                vs->n++;
                break;
            }
            seg = next ? next + 1 : NULL;
        }
        p = (*end == ',') ? end + 1 : end;
        if (*end && *end != ',') break;
    }
    return any;
}

int lane_voice_scope(const lane_t *ln, void *ctx)
{
    const lane_voice_scope_t *vs = ctx;
    if (!ln || !vs || strcmp(ln->target, "synth") != 0) return 0;
    const size_t pl = strlen(ln->param);
    if (!pl) return 0;
    for (int i = 0; i < vs->n; i++) {
        for (const char *k = vs->lo[i]; k < vs->hi[i];) {
            const char *e = k;
            while (e < vs->hi[i] && *e != ',') e++;
            if ((size_t)(e - k) == pl && memcmp(k, ln->param, pl) == 0) return 1;
            k = e + 1;
        }
    }
    return 0;
}
