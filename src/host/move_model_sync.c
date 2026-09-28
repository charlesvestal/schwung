/* move_model_sync.c -- see move_model_sync.h. */
#define _GNU_SOURCE
#include "move_model_sync.h"

#include <stdatomic.h>
#include <time.h>

#include "move_model.h"
#include "edit_follow.h"
#include "edit_gesture.h"
#include <stdio.h>
#include <string.h>
#include "shadow_chain_mgmt.h"
#include "shadow_set_pages.h"

/* Move rewrites Settings.json within ~12 ms of the document swap completing
 * (measured 2026-09-28). A dedupe ("same set as before") is only trusted from
 * a read taken this long after the edge; an earlier one may still see the
 * outgoing index. */
#define SETTLE_MS 300

static shadow_control_t **g_ctl;
static atomic_int g_active;
static atomic_uint g_gen;
static atomic_ullong g_edge_ms;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int move_model_sync_active(void) { return atomic_load(&g_active); }
uint32_t move_model_sync_gen(void) { return atomic_load(&g_active) ? atomic_load(&g_gen) : 0; }

int move_model_sync_settled(void)
{
    return atomic_load(&g_active) && now_ms() - atomic_load(&g_edge_ms) >= SETTLE_MS;
}

int move_model_sync_misaligned(void)
{
    shadow_control_t *ctl = g_ctl ? *g_ctl : NULL;
    return atomic_load(&g_active) && ctl && ctl->move_doc_gen != ctl->set_doc_gen;
}

/* ---- AUTOMATION FOLLOWS MOVE'S EDITS ------------------------------------
 *
 * Two lock-free single-producer rings:
 *   intents  SPI callback -> this thread (what the user asked Move to do);
 *   commands this thread -> SPI callback (lane verbs for the chain, which only
 *            the callback may touch).
 * edit_follow.c decides; see edit_follow.h for the rules. */
#define IQ_N 32
#define CQ_N 64
typedef struct { int slot; char key[24]; char val[104]; } cmd_t;
static ef_intent_t g_iq[IQ_N];
static atomic_uint g_iq_w, g_iq_r;
static cmd_t g_cq[CQ_N];
static atomic_uint g_cq_w, g_cq_r;

static void push_intent(const ef_intent_t *in)          /* SPI thread */
{
    unsigned w = atomic_load_explicit(&g_iq_w, memory_order_relaxed);
    if (w - atomic_load_explicit(&g_iq_r, memory_order_acquire) >= IQ_N) return;
    g_iq[w % IQ_N] = *in;
    atomic_store_explicit(&g_iq_w, w + 1, memory_order_release);
}
static void drain_intents(void)                         /* model thread */
{
    unsigned r = atomic_load_explicit(&g_iq_r, memory_order_relaxed);
    while (r != atomic_load_explicit(&g_iq_w, memory_order_acquire)) {
        edit_follow_intent(&g_iq[r % IQ_N]);
        atomic_store_explicit(&g_iq_r, ++r, memory_order_release);
    }
}
static void enqueue_cmd(void *ctx, int slot, const char *key, const char *val)   /* model thread */
{
    (void)ctx;
    unsigned w = atomic_load_explicit(&g_cq_w, memory_order_relaxed);
    if (w - atomic_load_explicit(&g_cq_r, memory_order_acquire) >= CQ_N) return;
    cmd_t *c = &g_cq[w % CQ_N];
    c->slot = slot;
    snprintf(c->key, sizeof c->key, "%s", key);
    snprintf(c->val, sizeof c->val, "%s", val);
    atomic_store_explicit(&g_cq_w, w + 1, memory_order_release);
}
int move_model_sync_pop_cmd(int *slot, char *key, int klen, char *val, int vlen)   /* SPI thread */
{
    unsigned r = atomic_load_explicit(&g_cq_r, memory_order_relaxed);
    if (r == atomic_load_explicit(&g_cq_w, memory_order_acquire)) return 0;
    const cmd_t *c = &g_cq[r % CQ_N];
    *slot = c->slot;
    snprintf(key, (size_t)klen, "%s", c->key);
    snprintf(val, (size_t)vlen, "%s", c->val);
    atomic_store_explicit(&g_cq_r, r + 1, memory_order_release);
    return 1;
}

static void edited_notes(ef_notes_t *nn, ef_notes_t *pn)
{
    nn->n = move_model_edited_notes(0, &nn->notes, &nn->ref);
    pn->n = move_model_edited_notes(1, &pn->notes, &pn->ref);
}

/* RT: one cable-0 MIDI_IN event Move is being given. The positions are taken
 * NOW, from the model's last snapshot: the source's page may be scrolled away
 * before the destination is pressed, and a paste of a clip's step from page 1
 * to page 3 is the ordinary case, not an edge. */
static edit_gesture_t g_gest = { 0, 0, 0, -1, 0 };
static struct { int valid, track, slot; uint64_t clip_id; double phase, len; } g_src;

static int button_phase(const move_model_t *m, int button, int page, int *track, int *slot,
                        uint64_t *clip_id, uint32_t *content, double *phase, double *len)
{
    if (!m->valid || m->selected_track < 0 || !(m->step_beats > 0.0)) return 0;
    const mm_track_t *T = &m->track[m->selected_track];
    const int cs = (T->mode == 1) ? T->playing_slot : -1;
    if (cs < 0 || cs >= MM_SLOTS || !T->slot[cs].exists) return 0;
    const mm_clip_t *c = &T->slot[cs];
    *track = m->selected_track;
    *slot = cs;
    *clip_id = c->clip_id;
    *content = mm_clip_state_hash(c);
    if (page) {
        *len = (m->step_triplet ? 12 : 16) * m->step_beats;
        *phase = button * *len;
    } else {
        if (c->scroll < 0.0) return 0;
        int step = button;
        if (m->step_triplet) {
            if (button % 4 == 3) return 0;            /* Move's dead triplet button */
            step = button - button / 4;
        }
        *len = m->step_beats;
        *phase = c->scroll + step * m->step_beats;
    }
    return 1;
}

void move_model_sync_on_midi(uint8_t status, uint8_t d1, uint8_t d2)
{
    if (!atomic_load_explicit(&g_active, memory_order_relaxed)) return;
    eg_intent_t e;
    if (!edit_gesture_on_event(&g_gest, status, d1, d2, &e)) return;
    static move_model_t m;                               /* SPI thread only */
    if (!move_model_get(&m)) return;
    int t, s;
    uint64_t id;
    uint32_t content;
    double ph, len;
    ef_intent_t in;
    memset(&in, 0, sizeof in);
    in.t_ms = now_ms();
    if (e.kind == EG_SOURCE) {
        g_src.valid = button_phase(&m, e.src, e.page, &g_src.track, &g_src.slot, &g_src.clip_id,
                                   &content, &g_src.phase, &g_src.len);
        return;
    }
    if (e.kind == EG_PASTE) {
        const int ok = g_src.valid && button_phase(&m, e.dst, e.page, &t, &s, &id, &content, &ph, &len);
        g_src.valid = 0;
        /* One clip: a paste across clips or tracks is not one this mirrors. */
        if (!ok || t != g_src.track || s != g_src.slot || id != g_src.clip_id) return;
        in.kind = EF_PASTE;
        in.src = g_src.phase;
        in.dst = ph;
        in.len = len;
    } else if (e.kind == EG_DOUBLE) {
        /* Move's Double Loop is a paste of the loop onto the new half. */
        if (!button_phase(&m, 0, 1, &t, &s, &id, &content, &ph, &len)) return;
        const mm_clip_t *c = &m.track[t].slot[s];
        const double ls = c->loop_on ? c->loop_start : c->region_start;
        const double le = c->loop_on ? c->loop_end : c->region_end;
        if (!(le - ls > 0.0)) return;
        in.kind = EF_DOUBLE;
        in.src = ls;
        in.dst = le;
        in.len = le - ls;
    } else {
        if (!button_phase(&m, 0, 1, &t, &s, &id, &content, &ph, &len)) return;
        in.kind = (e.kind == EG_REDO) ? EF_REDO : EF_UNDO;
    }
    in.track = t;
    in.slot = s;
    in.clip_id = id;
    in.pre_hash = content;
    push_intent(&in);
}

static int mixer_complete(const move_model_t *m)
{
    for (int t = 0; t < MM_TRACKS; t++)
        if (!m->track[t].mixer_valid) return 0;
    return 1;
}

static void on_change(const move_model_t *now, const move_model_t *prev)
{
    if (!now->valid) return;
    shadow_control_t *ctl = g_ctl ? *g_ctl : NULL;
    int edge = !prev->valid || now->doc_gen != prev->doc_gen;

    if (edge) {
        atomic_store(&g_edge_ms, now_ms());
        atomic_store(&g_gen, now->doc_gen);
        atomic_store(&g_active, 1);
        if (ctl) {
            ctl->move_doc_gen = now->doc_gen;
            ctl->move_model_ready = 1;
        }
        /* A new document: take all four as levels. */
        if (mixer_complete(now)) {
            int mu[4], so[4];
            for (int t = 0; t < 4; t++) { mu[t] = now->track[t].muted; so[t] = now->track[t].soloed; }
            shadow_apply_mix_state(mu, so);
        }
        /* Name it now rather than at the next 1.4 s scan. The worker keeps
         * polling every tick until the generations agree, which covers a read
         * that races Move's Settings.json rewrite. */
        shadow_poll_current_set();
        edit_follow_on_change(now, prev, NULL, NULL, now_ms(), enqueue_cmd, NULL);   /* resets */
        return;
    }

    {   /* clips deleted, restored by Undo, copied; pastes and their undo */
        ef_notes_t nn, pn;
        drain_intents();
        edited_notes(&nn, &pn);
        edit_follow_on_change(now, prev, &nn, &pn, now_ms(), enqueue_cmd, NULL);
    }

    /* Same document: edges only, so Schwung's own slot-mute controls hold
     * between Move gestures. Solo before mute is irrelevant -- Move's solo is
     * exclusive and shadow_apply_solo mirrors that. */
    for (int t = 0; t < MM_TRACKS; t++) {
        const mm_track_t *a = &prev->track[t], *b = &now->track[t];
        if (!a->mixer_valid || !b->mixer_valid) continue;
        if (a->soloed != b->soloed) shadow_apply_solo(t, b->soloed);
        if (a->muted != b->muted) shadow_apply_mute(t, b->muted);
    }
}

static void on_tick(const move_model_t *now)
{
    if (!atomic_load_explicit(&g_active, memory_order_relaxed)) return;
    ef_notes_t nn, pn;
    drain_intents();
    edited_notes(&nn, &pn);
    edit_follow_tick(now, &nn, &pn, now_ms(), enqueue_cmd, NULL);
}

void move_model_sync_init(shadow_control_t **control)
{
    g_ctl = control;
    move_model_set_tick_hook(on_tick);
    move_model_set_listener(on_change);
}
