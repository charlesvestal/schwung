/* move_model_sync.c driven by synthetic model snapshots: LEVELS on a new
 * document, EDGES within one, the set poll on the edge, and the generation
 * handshake that gates autosave. The mutators are recording stubs. */
#define _GNU_SOURCE   /* CLOCK_MONOTONIC under -std=c11 on Linux */
#include <stdio.h>
#include <string.h>
#include "move_model.h"
#include "move_model_sync.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---- stubs for what move_model_sync.c calls ---------------------------- */
static move_model_listener_fn g_fn;
void move_model_set_listener(move_model_listener_fn fn) { g_fn = fn; }
static int n_mix, mix_mu[4], mix_so[4], n_mute, n_solo, n_poll, last_slot, last_val;
void shadow_apply_mix_state(const int muted[4], const int soloed[4])
{
    n_mix++;
    memcpy(mix_mu, muted, sizeof mix_mu);
    memcpy(mix_so, soloed, sizeof mix_so);
}
void shadow_apply_mute(int slot, int v) { n_mute++; last_slot = slot; last_val = v; }
void shadow_apply_solo(int slot, int v) { n_solo++; last_slot = slot; last_val = v; }
void shadow_poll_current_set(void) { n_poll++; }
void shadow_log(const char *m) { (void)m; }
/* The reader's liveness: "just published" unless a test says otherwise. */
#include <time.h>
static long long fake_pub_age_ms = 0;   /* -1: never published */
uint64_t move_model_last_publish_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    return fake_pub_age_ms < 0 ? 0 : now - (uint64_t)fake_pub_age_ms;
}
/* Mix changes are POSTED by the reader and applied on the SPI thread: a
 * listener call is followed by the drain, as the shim's frame would. */
#define FIRE(a, b) do { g_fn((a), (b)); move_model_sync_apply_pending(); } while (0)

static void reset(void) { n_mix = n_mute = n_solo = n_poll = 0; last_slot = last_val = -1; }

static move_model_t doc(uint32_t gen)
{
    move_model_t m;
    memset(&m, 0, sizeof m);
    m.valid = 1;
    m.doc_gen = gen;
    for (int t = 0; t < MM_TRACKS; t++) m.track[t].mixer_valid = 1;
    return m;
}

int main(void)
{
    static shadow_control_t ctl;
    shadow_control_t *ctlp = &ctl;
    move_model_sync_init(&ctlp);
    CHECK(g_fn != NULL);
    CHECK(!move_model_sync_active());
    CHECK(move_model_sync_gen() == 0);      /* not active: reads as aligned (0 == 0) */
    CHECK(!move_model_sync_misaligned());

    /* First valid snapshot after boot: a new document -> all four as LEVELS,
     * an immediate set poll, and the generation published. */
    move_model_t none;
    memset(&none, 0, sizeof none);
    move_model_t a = doc(1);
    a.track[1].muted = 1;
    a.track[2].soloed = 1;
    reset();
    FIRE(&a, &none);
    CHECK(n_mix == 1 && mix_mu[1] == 1 && mix_mu[0] == 0 && mix_so[2] == 1);
    CHECK(n_poll == 1);
    CHECK(n_mute == 0 && n_solo == 0);
    CHECK(move_model_sync_active());
    CHECK(ctl.move_model_ready == 1 && ctl.move_doc_gen == 1);
    CHECK(move_model_sync_misaligned());    /* set_doc_gen still 0 */
    CHECK(!move_model_sync_settled());       /* the edge was just now */

    /* Within the same document: EDGES only. Muting track 4 applies exactly
     * that, and does not re-level the rest (Schwung's own slot mute holds). */
    move_model_t b = a;
    b.track[3].muted = 1;
    reset();
    FIRE(&b, &a);
    CHECK(n_mix == 0 && n_poll == 0);
    CHECK(n_mute == 1 && last_slot == 3 && last_val == 1);

    /* An unrelated change (a clip, say) moves no mixer state at all. */
    move_model_t c = b;
    c.track[0].slot[0].exists = 1;
    reset();
    FIRE(&c, &b);
    CHECK(n_mix == 0 && n_mute == 0 && n_solo == 0 && n_poll == 0);

    /* Exclusive solo moving from track 3 to track 1: two edges. */
    move_model_t d = c;
    d.track[2].soloed = 0;
    d.track[0].soloed = 1;
    reset();
    FIRE(&d, &c);
    CHECK(n_solo == 2);

    /* A track whose mixer was not read contributes no edge. */
    move_model_t e = d, f = d;
    e.track[1].mixer_valid = 0;
    f.track[1].mixer_valid = 0;
    f.track[1].muted = 0;
    reset();
    FIRE(&f, &e);
    CHECK(n_mute == 0);

    /* A set load: new generation -> levels again, and another poll. */
    move_model_t g = doc(2);
    reset();
    FIRE(&g, &d);
    CHECK(n_mix == 1 && n_poll == 1 && ctl.move_doc_gen == 2);

    /* ...but an incomplete mixer is never taken as a level. */
    move_model_t h = doc(3);
    h.track[0].mixer_valid = 0;
    reset();
    FIRE(&h, &g);
    CHECK(n_mix == 0 && n_poll == 1);

    /* Alignment is the two generations agreeing. */
    ctl.set_doc_gen = 3;
    CHECK(!move_model_sync_misaligned());

    /* An invalid snapshot changes nothing. */
    move_model_t bad = doc(9);
    bad.valid = 0;
    reset();
    FIRE(&bad, &h);
    CHECK(n_mix == 0 && n_poll == 0 && ctl.move_doc_gen == 3);

    /* ---- the reader posts, the SPI thread applies ----------------------- */
    {
        move_model_t p1 = doc(7), p2;
        FIRE(&p1, &h);
        reset();
        p2 = p1; p2.track[2].muted = 1;
        g_fn(&p2, &p1);                                   /* posted only */
        CHECK(n_mute == 0);
        move_model_sync_apply_pending();                  /* ...applied here */
        CHECK(n_mute == 1 && last_slot == 2 && last_val == 1);
    }

    /* ---- ACTIVE MEANS LIVE ---------------------------------------------- */
    CHECK(move_model_sync_active());
    fake_pub_age_ms = 5000;                               /* reader stopped 5 s ago */
    CHECK(!move_model_sync_active());
    CHECK(move_model_sync_gen() == 0);
    CHECK(!move_model_sync_misaligned());                 /* a dead model gates nothing */
    move_model_sync_housekeep();
    CHECK(ctl.move_model_ready == 0);                     /* ...and the UI is told */
    fake_pub_age_ms = 0;
    move_model_sync_housekeep();
    CHECK(ctl.move_model_ready == 1);
    fake_pub_age_ms = -1;
    CHECK(!move_model_sync_active());                     /* never published */
    fake_pub_age_ms = 0;

    if (fails) { printf("test_move_model_sync: %d FAILED\n", fails); return 1; }
    printf("test_move_model_sync: PASS\n");
    return 0;
}
