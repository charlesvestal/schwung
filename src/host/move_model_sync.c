/* move_model_sync.c -- see move_model_sync.h. */
#define _GNU_SOURCE
#include "move_model_sync.h"

#include <stdatomic.h>
#include <time.h>

#include "move_model.h"
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
        return;
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

void move_model_sync_init(shadow_control_t **control)
{
    g_ctl = control;
    move_model_set_listener(on_change);
}
