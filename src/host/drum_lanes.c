/* drum_lanes.c -- see drum_lanes.h. */
#include "drum_lanes.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void off(dl_voice_t *v, int lane, dl_emit_fn emit, void *ctx)
{
    if (!v->active) return;
    emit(ctx, (uint8_t)(0x80 | lane), v->note, 0);
    v->active = 0;
}

void dl_all_off(dl_track_t *st, dl_emit_fn emit, void *ctx)
{
    for (int k = 0; k < DL_LANES; k++) off(&st->v[k], k, emit, ctx);
    st->clip_id = 0;
}

static int clamp7(double x)
{
    long r = lround(x);
    return r < 0 ? 0 : r > 127 ? 127 : (int)r;
}

/* Start every note whose start lies in [a, b), `frac0` beats into the block. */
static void starts_in(dl_track_t *st, const mm_play_clip_t *clip, double a, double b,
                      double frac0, int base_note, dl_emit_fn emit, void *ctx)
{
    for (int i = 0; i < clip->n; i++) {
        const mm_play_note_t *n = &clip->note[i];
        if (!(n->start >= a && n->start < b)) continue;
        const int lane = n->pitch - DL_FIRST_PAD;
        if (lane < 0 || lane >= DL_LANES) continue;
        dl_voice_t *v = &st->v[lane];
        off(v, lane, emit, ctx);                 /* a lane is monophonic */
        const int vel = clamp7(n->vel);
        v->note = (uint8_t)clamp7(base_note + n->pitch_offset);
        emit(ctx, (uint8_t)(0x90 | lane), v->note, (uint8_t)(vel ? vel : 1));
        v->active = 1;
        /* measured from the start of THIS block; the tail below makes it next's */
        v->remaining = frac0 + (n->start - a) + n->dur;
    }
}

void dl_block(dl_track_t *st, const mm_play_clip_t *clip, dl_window_t w,
              double pos0, double blk, int base_note,
              dl_emit_fn emit, void *ctx)
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
        starts_in(st, clip, pos0, w.le, 0.0, base_note, emit, ctx);
        starts_in(st, clip, w.ls, w.ls + (end - w.le), w.le - pos0, base_note, emit, ctx);
    } else {
        starts_in(st, clip, pos0, end, 0.0, base_note, emit, ctx);
    }

    /* 3. Remaining time is now measured from the next block's start. */
    for (int k = 0; k < DL_LANES; k++)
        if (st->v[k].active) st->v[k].remaining -= blk;
}

/* ---- LIVE --------------------------------------------------------------- */

void dl_live_all_off(dl_live_t *st, dl_emit_slot_fn emit)
{
    for (int k = 0; k < DL_LANES; k++) {
        if (st->id[k] && st->slot[k] >= 0) emit(st->slot[k], (uint8_t)(0x80 | k), st->note[k], 0);
        st->id[k] = 0;
    }
}

void dl_live_ingest(dl_live_t *st, const mm_live_rec_t *r, int n, int slot, int base_note,
                    dl_emit_slot_fn emit)
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
                if (st->id[k] == e->id) {
                    if (st->slot[k] >= 0) emit(st->slot[k], (uint8_t)(0x80 | k), st->note[k], 0);
                    st->id[k] = 0;
                }
            continue;
        }
        if (e->kind != MM_LIVE_KIND_ON || e->id <= st->last_on) continue;
        if (e->id > newest) newest = e->id;
        const int lane = e->a - DL_FIRST_PAD;
        if (slot < 0 || lane < 0 || lane >= DL_LANES) continue;   /* seen, not routed */
        double pitch = 0.0;
        for (int j = 0; j < n; j++)
            if (r[j].kind == MM_LIVE_KIND_PNCC && r[j].a == -2 && r[j].id == e->id)
                pitch = r[j].b / (8191.0 / 48.0);
        if (st->id[lane] && st->slot[lane] >= 0)            /* a lane is monophonic */
            emit(st->slot[lane], (uint8_t)(0x80 | lane), st->note[lane], 0);
        const int vel = clamp7(e->b);
        st->note[lane] = (uint8_t)clamp7(base_note + pitch);
        st->slot[lane] = (int8_t)slot;
        st->id[lane] = e->id;
        emit(slot, (uint8_t)(0x90 | lane), st->note[lane], (uint8_t)(vel ? vel : 1));
    }
    st->last_on = newest;
}

/* ---- prototype config ------------------------------------------------- */

int dl_cfg_slot[MM_TRACKS] = { -1, -1, -1, -1 };
int dl_cfg_base[MM_TRACKS] = { 60, 60, 60, 60 };

void dl_config_parse(const char *text, int slot[MM_TRACKS], int base[MM_TRACKS])
{
    for (int t = 0; t < MM_TRACKS; t++) { slot[t] = -1; base[t] = 60; }
    const char *p = text;
    while (p && *p) {
        /* One LINE at a time: sscanf's %d skips newlines, so a two-field line
         * would take the next line's track number as its base note. */
        char line[64];
        const char *nl = strchr(p, '\n');
        size_t ln = nl ? (size_t)(nl - p) : strlen(p);
        if (ln >= sizeof line) ln = sizeof line - 1;
        memcpy(line, p, ln);
        line[ln] = '\0';
        int tr = 0, sl = 0, bn = 60;
        const int got = sscanf(line, "%d %d %d", &tr, &sl, &bn);
        if (got >= 2 && tr >= 1 && tr <= MM_TRACKS && sl >= 1 && sl <= MM_TRACKS) {
            slot[tr - 1] = sl - 1;
            base[tr - 1] = (got >= 3 && bn >= 0 && bn <= 127) ? bn : 60;
        }
        p = strchr(p, '\n');
        if (p) p++;
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
    int slot[MM_TRACKS], base[MM_TRACKS];
    dl_config_parse(buf, slot, base);
    for (int t = 0; t < MM_TRACKS; t++) {
        __atomic_store_n(&dl_cfg_base[t], base[t], __ATOMIC_RELAXED);
        __atomic_store_n(&dl_cfg_slot[t], slot[t], __ATOMIC_RELEASE);
    }
}
