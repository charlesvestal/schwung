/* move_model_sync.c -- see move_model_sync.h. */
#define _GNU_SOURCE
#include "move_model_sync.h"

#include <stdatomic.h>
#include <time.h>

#include "move_model.h"
#include "edit_follow.h"
#include "edit_gesture.h"
#include "undo_timeline.h"
#include "lane_edit.h"
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

/* ACTIVE MEANS LIVE, not "worked once". The reader publishes every ~20 ms; a
 * model that has stopped (the Song object lost and not re-found) must hand
 * mute/solo and set detection back to the fallbacks rather than own them
 * while following nothing. */
#define LIVE_MS 2000
int move_model_sync_active(void)
{
    if (!atomic_load(&g_active)) return 0;
    const uint64_t last = move_model_last_publish_ms();
    return last && now_ms() - last < LIVE_MS;
}
uint32_t move_model_sync_gen(void) { return move_model_sync_active() ? atomic_load(&g_gen) : 0; }

int move_model_sync_settled(void)
{
    return move_model_sync_active() && now_ms() - atomic_load(&g_edge_ms) >= SETTLE_MS;
}

int move_model_sync_misaligned(void)
{
    shadow_control_t *ctl = g_ctl ? *g_ctl : NULL;
    return move_model_sync_active() && ctl && ctl->move_doc_gen != ctl->set_doc_gen;
}

/* ---- MIX CHANGES, applied on the SPI thread ------------------------------
 *
 * The same slot mute/solo flags are written by the SPI callback's own param
 * handler (slot:muted, slot:soloed -- E16, CC map, Slot Settings). Applying
 * Move's changes from this reader thread as well raced it: shadow_solo_count
 * could disagree with the flags. So the reader only POSTS; the SPI thread,
 * the one writer, applies (move_model_sync_apply_pending). */
enum { MOP_LEVELS = 1, MOP_MUTE, MOP_SOLO };
typedef struct { int op, t, v; int mu[4], so[4]; } mop_t;
#define MQ_N 16
static mop_t g_mq[MQ_N];
static atomic_uint g_mq_w, g_mq_r;
static void post_mix(const mop_t *m)                    /* reader thread */
{
    unsigned w = atomic_load_explicit(&g_mq_w, memory_order_relaxed);
    if (w - atomic_load_explicit(&g_mq_r, memory_order_acquire) >= MQ_N) return;
    g_mq[w % MQ_N] = *m;
    atomic_store_explicit(&g_mq_w, w + 1, memory_order_release);
}
void move_model_sync_apply_pending(void)                /* SPI thread */
{
    unsigned r = atomic_load_explicit(&g_mq_r, memory_order_relaxed);
    while (r != atomic_load_explicit(&g_mq_w, memory_order_acquire)) {
        const mop_t *m = &g_mq[r % MQ_N];
        if (m->op == MOP_LEVELS) shadow_apply_mix_state(m->mu, m->so);
        else if (m->op == MOP_SOLO) shadow_apply_solo(m->t, m->v);
        else if (m->op == MOP_MUTE) shadow_apply_mute(m->t, m->v);
        atomic_store_explicit(&g_mq_r, ++r, memory_order_release);
    }
}

/* ---- HOUSEKEEPING, from the shim worker every tick ----------------------- */
#define MISALIGN_GIVEUP_MS 15000
void move_model_sync_housekeep(void)
{
    shadow_control_t *ctl = g_ctl ? *g_ctl : NULL;
    if (!ctl) return;
    const int live = move_model_sync_active();
    /* The UI reads readiness from here: a stalled model stops owning the mix
     * and stops gating autosave there too. */
    if (ctl->move_model_ready != (uint8_t)live) ctl->move_model_ready = (uint8_t)live;
    /* A MISALIGNMENT THAT CAN NEVER RESOLVE MUST NOT GATE AUTOSAVE FOREVER.
     * The only ways out are a consumed set read or the UI's ack, and the
     * worker's poll publishes nothing if Settings.json is unreadable or names
     * no set dir -- then every edit of the session would be lost on reboot.
     * After 15 s with no set change pending, align to what Move has and say
     * so; that is where the pre-model behaviour would have saved anyway. */
    /* ...and ONLY when nothing was read since the load: a read that exists
     * is pending (the consume waits for it to settle), and forcing alignment
     * over it would let autosave run before the UI has switched sets. */
    const uint64_t edge = atomic_load(&g_edge_ms);
    if (live && ctl->move_doc_gen != ctl->set_doc_gen &&
        !(ctl->ui_flags & SHADOW_UI_FLAG_SET_CHANGED) &&
        shadow_set_pages_last_publish_ms() < edge &&
        now_ms() - edge > MISALIGN_GIVEUP_MS) {
        ctl->set_doc_gen = ctl->move_doc_gen;
        shadow_log("move_model: set alignment gave up after 15 s (no set read); autosave resumes");
    }
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
typedef struct { int slot; char key[24]; char val[MMS_CMD_VAL]; } cmd_t;
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

static edit_gesture_t g_gest = { 0, 0, 0, -1, 0 };      /* SPI thread only */

/* ---- ONE UNDO (undo_timeline.h) ---------------------------------------
 *
 * The timeline lives on this thread. The SPI callback only reads the answer
 * it published -- "Undo now would take (slot, jid)", or 0 for Move's -- and,
 * if it takes a press, posts which one back. A third ring carries that, the
 * chain's own-edit announcements and Record's arm edges. */
_Static_assert(LANE_EDIT_PLOCK == UT_PLOCK && LANE_EDIT_TAKE == UT_TAKE &&
               LANE_EDIT_CLEAR == UT_CLEAR && LANE_EDIT_OTHER == UT_EDIT &&
               LANE_EDIT_RESET == UT_RESET,
               "the chain's edit kinds are the timeline's");
enum { UE_EDIT = 1, UE_ARM, UE_UNDO, UE_REDO };
typedef struct { int type, slot, kind; uint32_t jid; uint64_t t_ms; } uev_t;
#define UQ_N 32
static uev_t g_uq[UQ_N];
static atomic_uint g_uq_w, g_uq_r;
static ut_t g_ut;
static atomic_ullong g_claim_undo, g_claim_redo;   /* (slot + 1) << 32 | jid, 0 = Move's */
static int g_undo_latch;                            /* SPI thread only */

static void push_uev(int type, int slot, uint32_t jid, int kind)   /* SPI thread */
{
    unsigned w = atomic_load_explicit(&g_uq_w, memory_order_relaxed);
    if (w - atomic_load_explicit(&g_uq_r, memory_order_acquire) >= UQ_N) return;
    uev_t *e = &g_uq[w % UQ_N];
    e->type = type; e->slot = slot; e->jid = jid; e->kind = kind; e->t_ms = now_ms();
    atomic_store_explicit(&g_uq_w, w + 1, memory_order_release);
}

void move_model_sync_on_lane_edit(int slot, uint32_t jid, int kind)
{
    if (atomic_load_explicit(&g_active, memory_order_relaxed)) push_uev(UE_EDIT, slot, jid, kind);
}

void move_model_sync_on_arm(int armed)
{
    if (atomic_load_explicit(&g_active, memory_order_relaxed)) push_uev(UE_ARM, -1, 0, armed);
}

static uint64_t pack_claim(int ok, int slot, uint32_t jid)
{
    return ok ? ((uint64_t)(slot + 1) << 32) | jid : 0;
}

static void undo_tick(const move_model_t *now)                     /* model thread */
{
    ut_hist_t h = { now->hist_valid,
                    { now->hist_undo_node, now->hist_undo_nbr },
                    { now->hist_redo_node, now->hist_redo_nbr } };
    ut_on_history(&g_ut, &h, now_ms(), enqueue_cmd, NULL);
    unsigned r = atomic_load_explicit(&g_uq_r, memory_order_relaxed);
    while (r != atomic_load_explicit(&g_uq_w, memory_order_acquire)) {
        const uev_t *e = &g_uq[r % UQ_N];
        switch (e->type) {
        case UE_EDIT: ut_on_schwung_edit(&g_ut, e->slot, e->jid, e->kind, e->t_ms); break;
        case UE_ARM:  ut_on_arm(&g_ut, e->kind, e->t_ms); break;
        case UE_UNDO: ut_take_undo(&g_ut, e->slot, e->jid, enqueue_cmd, NULL); break;
        case UE_REDO: ut_take_redo(&g_ut, e->slot, e->jid, enqueue_cmd, NULL); break;
        }
        atomic_store_explicit(&g_uq_r, ++r, memory_order_release);
    }
    int s = -1; uint32_t j = 0;
    const int u = now->hist_valid && ut_undo_target(&g_ut, &s, &j);
    atomic_store(&g_claim_undo, pack_claim(u, s, j));
    const int d = now->hist_valid && ut_redo_target(&g_ut, &s, &j);
    atomic_store(&g_claim_redo, pack_claim(d, s, j));
}

/* RT: an Undo press (Redo with Shift) that the timeline says is Schwung's.
 * Both edges are swallowed, latched on the press: Move must not see a lone
 * release for a press it never got. */
static int undo_claim(uint8_t d2)
{
    if (d2 > 0) {
        /* Every press decides afresh: a latch left by a release that never
         * came here (overtake began mid-press) must not swallow this one's. */
        g_undo_latch = 0;
        const int redo = g_gest.shift_held;
        uint64_t c = atomic_exchange(redo ? &g_claim_redo : &g_claim_undo, 0);
        if (!c) return 0;
        push_uev(redo ? UE_REDO : UE_UNDO, (int)(c >> 32) - 1, (uint32_t)c, 0);
        g_undo_latch = 1;
        return 1;
    }
    if (g_undo_latch) { g_undo_latch = 0; return 1; }
    return 0;
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

int move_model_sync_on_midi(uint8_t status, uint8_t d1, uint8_t d2)
{
    if (!atomic_load_explicit(&g_active, memory_order_relaxed)) return 0;
    if ((status & 0xF0) == 0xB0 && d1 == EG_CC_UNDO && undo_claim(d2)) return 1;
    eg_intent_t e;
    if (!edit_gesture_on_event(&g_gest, status, d1, d2, &e)) return 0;
    static move_model_t m;                               /* SPI thread only */
    move_model_get(&m);                                  /* torn: the last good snapshot */
    if (!m.valid) return 0;
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
        return 0;
    }
    if (e.kind == EG_PASTE) {
        const int ok = g_src.valid && button_phase(&m, e.dst, e.page, &t, &s, &id, &content, &ph, &len);
        g_src.valid = 0;
        /* One clip: a paste across clips or tracks is not one this mirrors. */
        if (!ok || t != g_src.track || s != g_src.slot || id != g_src.clip_id) return 0;
        in.kind = EF_PASTE;
        in.src = g_src.phase;
        in.dst = ph;
        in.len = len;
    } else if (e.kind == EG_DOUBLE) {
        /* Move's Double Loop is a paste of the loop onto the new half. */
        if (!button_phase(&m, 0, 1, &t, &s, &id, &content, &ph, &len)) return 0;
        const mm_clip_t *c = &m.track[t].slot[s];
        const double ls = c->loop_on ? c->loop_start : c->region_start;
        const double le = c->loop_on ? c->loop_end : c->region_end;
        if (!(le - ls > 0.0)) return 0;
        in.kind = EF_DOUBLE;
        in.src = ls;
        in.dst = le;
        in.len = le - ls;
    } else {
        /* Undo/Redo name no clip: Move's history is the whole document's,
         * and edit_follow matches the press against every journaled paste.
         * So it is posted even with nothing on screen (Session view). */
        in.kind = (e.kind == EG_REDO) ? EF_REDO : EF_UNDO;
        push_intent(&in);
        return 0;
    }
    in.track = t;
    in.slot = s;
    in.clip_id = id;
    in.pre_hash = content;
    push_intent(&in);
    return 0;
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
            mop_t m = { .op = MOP_LEVELS };
            for (int t = 0; t < 4; t++) { m.mu[t] = now->track[t].muted; m.so[t] = now->track[t].soloed; }
            post_mix(&m);
        }
        /* Name it now rather than at the next 1.4 s scan. The worker keeps
         * polling every tick until the generations agree, which covers a read
         * that races Move's Settings.json rewrite. */
        shadow_poll_current_set();
        edit_follow_on_change(now, prev, NULL, NULL, now_ms(), enqueue_cmd, NULL);   /* resets */
        ut_reset(&g_ut);              /* Move's history is the new document's */
        atomic_store(&g_claim_undo, 0);
        atomic_store(&g_claim_redo, 0);
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
        if (a->soloed != b->soloed) { mop_t m = { .op = MOP_SOLO, .t = t, .v = b->soloed }; post_mix(&m); }
        if (a->muted != b->muted) { mop_t m = { .op = MOP_MUTE, .t = t, .v = b->muted }; post_mix(&m); }
    }
}

static void on_tick(const move_model_t *now)
{
    if (!atomic_load_explicit(&g_active, memory_order_relaxed)) return;
    ef_notes_t nn, pn;
    drain_intents();
    edited_notes(&nn, &pn);
    edit_follow_tick(now, &nn, &pn, now_ms(), enqueue_cmd, NULL);
    undo_tick(now);
}

void move_model_sync_init(shadow_control_t **control)
{
    g_ctl = control;
    move_model_set_tick_hook(on_tick);
    move_model_set_listener(on_change);
}
