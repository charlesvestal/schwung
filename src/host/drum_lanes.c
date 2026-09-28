/* drum_lanes.c -- see drum_lanes.h. */
#include "drum_lanes.h"
#include <math.h>

static int clamp7(double x)
{
    long r = lround(x);
    return r < 0 ? 0 : r > 127 ? 127 : (int)r;
}

dl_out_t dl_out_for(int t, int midi_out_ep)
{
    dl_out_t o;
    if (midi_out_ep >= 0 && midi_out_ep < 16) { o.ch = midi_out_ep; o.plain = 0; }
    else                                      { o.ch = t & 15;      o.plain = 1; }
    return o;
}

static void bend(dl_emit_fn emit, void *ctx, int ch, long pb)
{
    if (pb < 0) pb = 0;
    if (pb > 16383) pb = 16383;
    emit(ctx, (uint8_t)(0xE0 | ch), (uint8_t)(pb & 0x7F), (uint8_t)(pb >> 7));
}

void dl_emit_on(dl_emit_fn emit, void *ctx, int ch, int note, int velocity, int has_pitch, double semis)
{
    const int vel = clamp7(velocity);
    ch &= 15;
    if (has_pitch) {
        emit(ctx, (uint8_t)(0xB0 | ch), DL_CC_PITCHED, 1);
        bend(emit, ctx, ch, 8192 + lround(semis * 8191.0 / DL_PB_SEMIS));
    }
    emit(ctx, (uint8_t)(0x90 | ch), (uint8_t)(note & 0x7F), (uint8_t)(vel ? vel : 1));
    if (has_pitch) emit(ctx, (uint8_t)(0xB0 | ch), DL_CC_PITCHED, 0);
}

void dl_emit_off(dl_emit_fn emit, void *ctx, int ch, int note, int has_pitch)
{
    ch &= 15;
    emit(ctx, (uint8_t)(0x80 | ch), (uint8_t)(note & 0x7F), 0);
    if (has_pitch) bend(emit, ctx, ch, 8192);
}

/* ---- voices -------------------------------------------------------------- */

static void off(dl_voice_t *v, int lane, dl_emit_fn emit, void *ctx)
{
    if (!v->active) return;
    dl_emit_off(emit, ctx, v->ch, DL_FIRST_PAD + lane, v->pitched);
    v->active = 0;
}

/* A plain note we do not send took this lane: Move's NoteOn supersedes ours,
 * so only undo the bend. */
static void superseded(dl_voice_t *v, dl_emit_fn emit, void *ctx)
{
    if (!v->active) return;
    if (v->pitched) bend(emit, ctx, v->ch, 8192);
    v->active = 0;
}

/* Start a note on a lane, or return 0 when it is not ours to send. */
static int start(dl_voice_t *v, int lane, int vel, int has_pitch, double semis, dl_out_t out,
                 dl_emit_fn emit, void *ctx)
{
    if (!has_pitch && !out.plain) { superseded(v, emit, ctx); return 0; }
    off(v, lane, emit, ctx);                     /* a lane is monophonic */
    dl_emit_on(emit, ctx, out.ch, DL_FIRST_PAD + lane, vel, has_pitch, semis);
    v->active = 1;
    v->pitched = (uint8_t)(has_pitch != 0);
    v->ch = (uint8_t)(out.ch & 15);
    return 1;
}

/* ---- SEQUENCED ----------------------------------------------------------- */

void dl_all_off(dl_track_t *st, dl_emit_fn emit, void *ctx)
{
    for (int k = 0; k < DL_LANES; k++) off(&st->v[k], k, emit, ctx);
    st->clip_id = 0;
}

/* Start every note whose start lies in [a, b), `frac0` beats into the block. */
static void starts_in(dl_track_t *st, const mm_play_clip_t *clip, double a, double b,
                      double frac0, dl_out_t out, dl_emit_fn emit, void *ctx)
{
    for (int i = 0; i < clip->n; i++) {
        const mm_play_note_t *n = &clip->note[i];
        if (!(n->start >= a && n->start < b)) continue;
        const int lane = n->pitch - DL_FIRST_PAD;
        if (lane < 0 || lane >= DL_LANES) continue;
        dl_voice_t *v = &st->v[lane];
        if (!start(v, lane, (int)lroundf(n->vel), n->has_pitch, n->pitch_offset, out, emit, ctx))
            continue;
        /* measured from the start of THIS block; the tail below makes it next's */
        v->remaining = frac0 + (n->start - a) + n->dur;
    }
}

void dl_block(dl_track_t *st, const mm_play_clip_t *clip, dl_window_t w,
              double pos0, double blk, dl_out_t out, dl_emit_fn emit, void *ctx)
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
        starts_in(st, clip, pos0, w.le, 0.0, out, emit, ctx);
        starts_in(st, clip, w.ls, w.ls + (end - w.le), w.le - pos0, out, emit, ctx);
    } else {
        starts_in(st, clip, pos0, end, 0.0, out, emit, ctx);
    }

    /* 3. Remaining time is now measured from the next block's start. */
    for (int k = 0; k < DL_LANES; k++)
        if (st->v[k].active) st->v[k].remaining -= blk;
}

/* ---- LIVE ---------------------------------------------------------------- */

void dl_live_all_off(dl_live_t *st, dl_emit_fn emit, void *ctx)
{
    for (int k = 0; k < DL_LANES; k++) {
        off(&st->v[k], k, emit, ctx);
        st->id[k] = 0;
    }
}

void dl_live_ingest(dl_live_t *st, const mm_live_rec_t *r, int n, int routed, dl_out_t out,
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
                if (st->id[k] == e->id) { off(&st->v[k], k, emit, ctx); st->id[k] = 0; }
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
        st->id[lane] = start(&st->v[lane], lane, (int)lroundf(e->b), has_pitch, pitch, out, emit, ctx)
                       ? e->id : 0;
    }
    st->last_on = newest;
}

/* ---- which tracks -------------------------------------------------------- */

void dl_tracks_wanted(int nslots, const int *slot_rx, const int *slot_on,
                      const int track_ch[MM_TRACKS], int on[MM_TRACKS])
{
    for (int t = 0; t < MM_TRACKS; t++) {
        on[t] = 0;
        for (int i = 0; i < nslots; i++)
            if (slot_on[i] && (slot_rx[i] < 0 || slot_rx[i] == track_ch[t])) on[t] = 1;
    }
}
