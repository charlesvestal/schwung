/* drum_lanes.c -- see drum_lanes.h. */
#include "drum_lanes.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int clamp7(double x)
{
    long r = lround(x);
    return r < 0 ? 0 : r > 127 ? 127 : (int)r;
}

void dl_emit_on(dl_emit_fn emit, void *ctx, int lane, int velocity, int has_pitch, double semis)
{
    long pb = 8192;
    if (has_pitch) {
        pb = 8192 + lround(semis * 8191.0 / DL_PB_SEMIS);
        if (pb < 0) pb = 0;
        if (pb > 16383) pb = 16383;
    }
    const int vel = clamp7(velocity);
    emit(ctx, (uint8_t)(0xB0 | lane), DL_CC_PITCHED, has_pitch ? 1 : 0);
    emit(ctx, (uint8_t)(0xE0 | lane), (uint8_t)(pb & 0x7F), (uint8_t)(pb >> 7));
    emit(ctx, (uint8_t)(0x90 | lane), (uint8_t)(DL_FIRST_PAD + lane), (uint8_t)(vel ? vel : 1));
}

void dl_emit_off(dl_emit_fn emit, void *ctx, int lane)
{
    emit(ctx, (uint8_t)(0x80 | lane), (uint8_t)(DL_FIRST_PAD + lane), 0);
}

/* ---- SEQUENCED ----------------------------------------------------------- */

static void off(dl_voice_t *v, int lane, dl_emit_fn emit, void *ctx)
{
    if (!v->active) return;
    dl_emit_off(emit, ctx, lane);
    v->active = 0;
}

void dl_all_off(dl_track_t *st, dl_emit_fn emit, void *ctx)
{
    for (int k = 0; k < DL_LANES; k++) off(&st->v[k], k, emit, ctx);
    st->clip_id = 0;
}

/* Start every note whose start lies in [a, b), `frac0` beats into the block. */
static void starts_in(dl_track_t *st, const mm_play_clip_t *clip, double a, double b,
                      double frac0, dl_emit_fn emit, void *ctx)
{
    for (int i = 0; i < clip->n; i++) {
        const mm_play_note_t *n = &clip->note[i];
        if (!(n->start >= a && n->start < b)) continue;
        const int lane = n->pitch - DL_FIRST_PAD;
        if (lane < 0 || lane >= DL_LANES) continue;
        dl_voice_t *v = &st->v[lane];
        off(v, lane, emit, ctx);                 /* a lane is monophonic */
        dl_emit_on(emit, ctx, lane, (int)lroundf(n->vel), n->has_pitch, n->pitch_offset);
        v->active = 1;
        /* measured from the start of THIS block; the tail below makes it next's */
        v->remaining = frac0 + (n->start - a) + n->dur;
    }
}

void dl_block(dl_track_t *st, const mm_play_clip_t *clip, dl_window_t w,
              double pos0, double blk, dl_emit_fn emit, void *ctx)
{
    if (!clip || !clip->valid || !(pos0 >= 0.0) || !(blk > 0.0) || !(w.le > w.ls)) {
        dl_all_off(st, emit, ctx);
        return;
    }
    if (st->clip_id != clip->clip_id) {
        dl_all_off(st, emit, ctx);
        st->clip_id = clip->clip_id;
    }

    /* 1. Notes that ended by the start of this block. At least one block
     *    long: an on and its off in one block would reach the synth together. */
    for (int k = 0; k < DL_LANES; k++)
        if (st->v[k].active && st->v[k].remaining <= 0.0) off(&st->v[k], k, emit, ctx);

    /* 2. Notes that start in this block's window, wrapping at the loop end. */
    const double end = pos0 + blk;
    if (w.loop && end > w.le && pos0 < w.le) {
        starts_in(st, clip, pos0, w.le, 0.0, emit, ctx);
        starts_in(st, clip, w.ls, w.ls + (end - w.le), w.le - pos0, emit, ctx);
    } else {
        starts_in(st, clip, pos0, end, 0.0, emit, ctx);
    }

    /* 3. Remaining time is now measured from the next block's start. */
    for (int k = 0; k < DL_LANES; k++)
        if (st->v[k].active) st->v[k].remaining -= blk;
}

/* ---- LIVE ---------------------------------------------------------------- */

void dl_live_all_off(dl_live_t *st, dl_emit_fn emit, void *ctx)
{
    for (int k = 0; k < DL_LANES; k++) {
        if (st->id[k]) dl_emit_off(emit, ctx, k);
        st->id[k] = 0;
    }
}

void dl_live_ingest(dl_live_t *st, const mm_live_rec_t *r, int n, int routed,
                    dl_emit_fn emit, void *ctx)
{
    if (!st->primed) {                     /* stale records from before we looked */
        for (int i = 0; i < n; i++)
            if (r[i].kind == MM_LIVE_KIND_ON && r[i].id > st->last_on) st->last_on = r[i].id;
        st->primed = 1;
        return;
    }
    /* Move restarted its note ids (a new engine): forget the high-water mark. */
    for (int i = 0; i < n; i++)
        if (r[i].kind == MM_LIVE_KIND_ON && r[i].id > 0 && r[i].id + 100000 < st->last_on) {
            st->last_on = 0;
            break;
        }
    int64_t newest = st->last_on;
    for (int i = 0; i < n; i++) {
        const mm_live_rec_t *e = &r[i];
        if (e->ep != MM_LIVE_EP_INPUT || e->id <= 0) continue;
        if (e->kind == MM_LIVE_KIND_OFF) {
            for (int k = 0; k < DL_LANES; k++)
                if (st->id[k] == e->id) { dl_emit_off(emit, ctx, k); st->id[k] = 0; }
            continue;
        }
        if (e->kind != MM_LIVE_KIND_ON || e->id <= st->last_on) continue;
        if (e->id > newest) newest = e->id;
        const int lane = e->a - DL_FIRST_PAD;
        if (!routed || lane < 0 || lane >= DL_LANES) continue;   /* seen, not routed */
        double pitch = 0.0;
        int has_pitch = 0;
        for (int j = 0; j < n; j++)
            if (r[j].kind == MM_LIVE_KIND_PNCC && r[j].a == -2 && r[j].id == e->id) {
                pitch = r[j].b / (8191.0 / 48.0);
                has_pitch = 1;
            }
        if (st->id[lane]) dl_emit_off(emit, ctx, lane);          /* a lane is monophonic */
        st->id[lane] = e->id;
        dl_emit_on(emit, ctx, lane, (int)lroundf(e->b), has_pitch, pitch);
    }
    st->last_on = newest;
}

/* ---- prototype config ---------------------------------------------------- */

int dl_cfg_on[MM_TRACKS];

void dl_config_parse(const char *text, int on[MM_TRACKS])
{
    for (int t = 0; t < MM_TRACKS; t++) on[t] = 0;
    const char *p = text;
    while (p && *p) {
        /* One LINE at a time: sscanf's %d skips newlines. */
        char line[64];
        const char *nl = strchr(p, '\n');
        size_t ln = nl ? (size_t)(nl - p) : strlen(p);
        if (ln >= sizeof line) ln = sizeof line - 1;
        memcpy(line, p, ln);
        line[ln] = '\0';
        int tr = 0;
        if (sscanf(line, "%d", &tr) == 1 && tr >= 1 && tr <= MM_TRACKS) on[tr - 1] = 1;
        p = nl ? nl + 1 : NULL;
    }
}

void dl_config_poll(void)
{
    static long long seen = -2;                 /* mtime seen; -1 = absent */
    struct stat st;
    long long now = (stat(DL_CONF_PATH, &st) == 0) ? (long long)st.st_mtime * 1000 + st.st_size : -1;
    if (now == seen) return;
    seen = now;
    char buf[512] = "";
    if (now >= 0) {
        FILE *f = fopen(DL_CONF_PATH, "r");
        if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
    }
    int on[MM_TRACKS];
    dl_config_parse(buf, on);
    for (int t = 0; t < MM_TRACKS; t++) __atomic_store_n(&dl_cfg_on[t], on[t], __ATOMIC_RELEASE);
}
