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

/* shim_worker.c: the clip-event channels the lanes' push loop consumes. */
void shim_worker_publish_clip_deleted(uint32_t mask);
void shim_worker_publish_clip_copy(int track, int src, int dst);

/* A clip is "gone" from a slot when it no longer exists there or the object
 * there is a different one (deleted and remade within one tick). */
static int clip_gone(const mm_clip_t *a, const mm_clip_t *b)
{
    return a->exists && (!b->exists || b->clip_id != a->clip_id);
}
static int clip_arrived(const mm_clip_t *a, const mm_clip_t *b)
{
    return b->exists && (!a->exists || b->clip_id != a->clip_id);
}
/* Move's Copy: a new clip whose notes and geometry equal a clip already on
 * the track. Note-less clips never match -- two empty clips are not a copy. */
static int clip_same_content(const mm_clip_t *x, const mm_clip_t *y)
{
    return x->notes_len > 0 && x->notes_len == y->notes_len && x->notes_hash == y->notes_hash &&
           x->region_start == y->region_start && x->region_end == y->region_end &&
           x->loop_start == y->loop_start && x->loop_end == y->loop_end && x->loop_on == y->loop_on;
}

/* Deletions and copies within one document, for the automation lanes: a
 * deleted clip ORPHANS its lanes (they must not play on whatever is made in
 * that slot next); a copied clip takes a copy of its source's lanes. */
static void clip_events(const move_model_t *now, const move_model_t *prev)
{
    uint32_t deleted = 0;
    for (int t = 0; t < MM_TRACKS; t++) {
        for (int s = 0; s < MM_SLOTS; s++) {
            const mm_clip_t *a = &prev->track[t].slot[s], *b = &now->track[t].slot[s];
            if (clip_gone(a, b)) deleted |= 1u << (t * 8 + s);
            if (!clip_arrived(a, b)) continue;
            for (int src = 0; src < MM_SLOTS; src++) {
                const mm_clip_t *p = &prev->track[t].slot[src];
                if (src == s || !p->exists) continue;
                if (!clip_same_content(p, b)) continue;
                shim_worker_publish_clip_copy(t, src, s);
                break;
            }
        }
    }
    if (deleted) shim_worker_publish_clip_deleted(deleted);
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
        return;
    }

    clip_events(now, prev);

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

void move_model_sync_init(shadow_control_t **control)
{
    g_ctl = control;
    move_model_set_listener(on_change);
}
