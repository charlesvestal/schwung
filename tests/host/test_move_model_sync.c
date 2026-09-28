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
static move_model_tick_fn g_tick;
void move_model_set_tick_hook(move_model_tick_fn fn) { g_tick = fn; }
int move_model_get(move_model_t *out) { memset(out, 0, sizeof *out); return 0; }
int move_model_edited_notes(int previous, const mm_note_t **notes, mm_clip_ref_t *ref)
{ (void)previous; if (notes) *notes = NULL; if (ref) memset(ref, 0, sizeof *ref); return -1; }
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

/* Clip events now travel as lane COMMANDS through the ring the SPI callback
 * drains: count what comes out. */
static uint32_t del_mask; static int n_del, n_copy, cp_t, cp_src, cp_dst;
static int n_jrn, jrn_slot; static char jrn_val[64];
static void drain_cmds(void)
{
    int slot; char k[24], v[MMS_CMD_VAL];
    while (move_model_sync_pop_cmd(&slot, k, sizeof k, v, sizeof v)) {
        if (!strcmp(k, "lanes:journal")) { n_jrn++; jrn_slot = slot; snprintf(jrn_val, sizeof jrn_val, "%s", v); }
        int t, s, a, b;
        if (!strcmp(k, "lanes:stash") && sscanf(v, "%d %d", &t, &s) == 2) { n_del++; del_mask |= 1u << (t * 8 + s); }
        if (!strcmp(k, "lanes:copy_clip") && sscanf(v, "%d %d", &a, &b) == 2) { n_copy++; cp_t = slot; cp_src = a; cp_dst = b; }
    }
}

static void drain_cmds(void);
static void reset(void)
{
    drain_cmds();
    n_mix = n_mute = n_solo = n_poll = n_del = n_copy = 0;
    last_slot = last_val = -1;
    del_mask = 0;
}
static mm_clip_t clip(uint64_t id, uint32_t hash)
{
    mm_clip_t c;
    memset(&c, 0, sizeof c);
    c.exists = 1; c.clip_id = id; c.notes_len = hash ? 40 : 0; c.notes_hash = hash;
    c.region_end = 4; c.loop_end = 4; c.loop_on = 1;
    return c;
}

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

    /* ---- clip events, within one document -------------------------------- */
    move_model_t p = doc(3), q;
    p.track[1].slot[0] = clip(100, 0xabc);
    p.track[1].slot[2] = clip(101, 0x777);
    /* delete track 2 slot 3 -> bit 1*8+2 */
    q = p; memset(&q.track[1].slot[2], 0, sizeof(mm_clip_t));
    reset(); FIRE(&q, &p); drain_cmds();
    CHECK(n_del == 1 && del_mask == (1u << 10) && n_copy == 0);
    /* deleted and REMADE in the same slot within one tick: still a deletion */
    q = p; q.track[1].slot[2] = clip(202, 0x777);
    reset(); FIRE(&q, &p); drain_cmds();
    CHECK(n_del == 1 && del_mask == (1u << 10));
    /* Move's Copy: slot 0 duplicated into slot 5 */
    q = p; q.track[1].slot[5] = clip(300, 0xabc);
    reset(); FIRE(&q, &p); drain_cmds();
    CHECK(n_copy == 1 && cp_t == 1 && cp_src == 0 && cp_dst == 5 && n_del == 0);
    /* a new clip with different notes is not a copy */
    q = p; q.track[1].slot[5] = clip(301, 0x999);
    reset(); FIRE(&q, &p); drain_cmds();
    CHECK(n_copy == 0);
    /* two EMPTY clips are not a copy either */
    move_model_t r = p; r.track[2].slot[0] = clip(400, 0);
    q = r; q.track[2].slot[1] = clip(401, 0);
    reset(); FIRE(&q, &r); drain_cmds();
    CHECK(n_copy == 0);
    /* an edit in place (same id, new notes) is neither */
    q = p; q.track[1].slot[0].notes_hash = 0xdef;
    reset(); FIRE(&q, &p); drain_cmds();
    CHECK(n_copy == 0 && n_del == 0);
    /* a SET LOAD replaces every id: never read as deletions */
    move_model_t z = doc(4);
    z.track[1].slot[0] = clip(900, 0xabc);
    reset(); FIRE(&z, &p); drain_cmds();
    CHECK(n_del == 0 && n_copy == 0 && n_mix == 1);

    /* ---- ONE UNDO: the press path, end to end ---------------------------
     * Move's stack top is (0x10, 1); Schwung p-locks on slot 2; Undo is
     * swallowed (both edges) and becomes that slot's journal undo. */
    {
        move_model_t u = z;
        u.hist_valid = 1; u.hist_undo_node = 0x10; u.hist_undo_nbr = 1;
        CHECK(g_tick != NULL);
        reset(); n_jrn = 0;
        g_tick(&u);
        CHECK(move_model_sync_on_midi(0xB0, 56, 127) == 0);          /* nothing of ours yet */
        CHECK(move_model_sync_on_midi(0xB0, 56, 0) == 0);
        move_model_sync_on_lane_edit(2, 0x80000001u, 1);
        g_tick(&u);
        CHECK(move_model_sync_on_midi(0xB0, 56, 127) == 1);          /* ours: swallowed */
        CHECK(move_model_sync_on_midi(0xB0, 56, 0) == 1);            /* and its release */
        g_tick(&u); drain_cmds();
        CHECK(n_jrn == 1 && jrn_slot == 2 && strcmp(jrn_val, "undo 2147483649") == 0);
        CHECK(move_model_sync_on_midi(0xB0, 56, 127) == 0);          /* next Undo is Move's */
        CHECK(move_model_sync_on_midi(0xB0, 56, 0) == 0);

        /* Shift+Undo is Redo, and ours comes back first. */
        CHECK(move_model_sync_on_midi(0xB0, 49, 127) == 0);          /* Shift reaches Move */
        CHECK(move_model_sync_on_midi(0xB0, 56, 127) == 1);
        CHECK(move_model_sync_on_midi(0xB0, 56, 0) == 1);
        CHECK(move_model_sync_on_midi(0xB0, 49, 0) == 0);
        g_tick(&u); drain_cmds();
        CHECK(n_jrn == 2 && strcmp(jrn_val, "redo 2147483649") == 0);

        /* Move makes an edit: Undo is Move's again. */
        move_model_t v2 = u; v2.hist_undo_node = 0x20; v2.hist_undo_nbr = 2;
        g_tick(&v2);
        CHECK(move_model_sync_on_midi(0xB0, 56, 127) == 0);
        CHECK(move_model_sync_on_midi(0xB0, 56, 0) == 0);

        /* An unreadable stack claims nothing, whatever the timeline holds. */
        move_model_sync_on_lane_edit(1, 0x80000001u, 1);
        move_model_t blind = v2; blind.hist_valid = 0;
        g_tick(&blind);
        CHECK(move_model_sync_on_midi(0xB0, 56, 127) == 0);
        CHECK(move_model_sync_on_midi(0xB0, 56, 0) == 0);
    }

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
