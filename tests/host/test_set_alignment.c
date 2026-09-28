/* Set alignment (shadow_set_pages.c): the SET_CHANGED / set_aligned handshake
 * that gates autosave while Schwung's per-set state catches up with a set
 * load. Driven through the real consume decision and ack, with the model's
 * generation and liveness stubbed. */
#include <stdio.h>
#include <string.h>
#include "shadow_set_pages.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---- the model, stubbed ------------------------------------------------- */
static int m_active = 1;
static uint32_t m_gen = 0;
int move_model_sync_active(void) { return m_active; }
uint32_t move_model_sync_gen(void) { return m_active ? m_gen : 0; }
int move_model_sync_settled(void) { return m_active; }
int move_model_sync_misaligned(void) { return 0; }

/* ---- what the file's other functions reach for -------------------------- */
static void s_log(const char *m) { (void)m; }
static int s_run(const char *const argv[]) { (void)argv; return 0; }
static void s_void(void) {}
static void s_slot(int s) { (void)s; }
static int s_ch(int c) { return c; }
static float s_tempo(const char *n) { (void)n; return 120.0f; }
static int s_mutes(const char *n, int m[4], int so[4]) { (void)n; (void)m; (void)so; return 0; }
float sampler_read_set_tempo(const char *n) { (void)n; return 0; }

static shadow_control_t ctl_s;
static shadow_control_t *ctl = &ctl_s;
static shadow_chain_slot_t slots[4];
static volatile int solo;

static int raised(void) { return (ctl->ui_flags & SHADOW_UI_FLAG_SET_CHANGED) != 0; }
static void ui_clear(void) { ctl->ui_flags &= (uint8_t)~SHADOW_UI_FLAG_SET_CHANGED; }
/* A read the model has had time to agree with. */
static int read_set(const char *name, uint32_t gen) { m_gen = gen; return shadow_set_pages_consume_read(name, name, gen, 1, 1000); }

int main(void)
{
    set_pages_host_t h = { s_log, s_log, s_void, s_run, s_void, s_mutes, s_tempo, s_slot, s_void, s_ch,
                           slots, &ctl, &solo };
    set_pages_init(&h);

    /* Boot: set A. The UI switches and acks the generation it read with the name. */
    ctl->move_doc_gen = 1;
    CHECK(read_set("A", 1) && raised(), "A raises SET_CHANGED");
    uint32_t handled = shadow_set_pages_published_gen();
    CHECK(handled == 1, "A is published as gen 1 (%u)", handled);
    ui_clear(); shadow_set_pages_ack_aligned(handled);
    CHECK(ctl->set_doc_gen == 1 && !raised(), "A aligned");

    /* THE MID-SWITCH LOAD. B is loaded; while the UI is switching to B
     * (it read "B" + gen 2), C is loaded. The UI's clear erases C's flag. */
    ctl->move_doc_gen = 2;
    read_set("B", 2);
    uint32_t handled_b = shadow_set_pages_published_gen();
    ctl->move_doc_gen = 3;
    read_set("C", 3);
    ui_clear();
    shadow_set_pages_ack_aligned(handled_b);
    CHECK(ctl->set_doc_gen == 2, "the ack names what was HANDLED (B), not the latest");
    CHECK(raised(), "C's SET_CHANGED is raised again -- it was never handled");
    CHECK(ctl->set_doc_gen != ctl->move_doc_gen, "and autosave stays gated until it is");
    uint32_t handled_c = shadow_set_pages_published_gen();
    ui_clear(); shadow_set_pages_ack_aligned(handled_c);
    CHECK(ctl->set_doc_gen == 3 && !raised(), "C handled and aligned");

    /* The old bug, directly: with C's flag lost, a same-name read must NOT
     * align C by itself while the UI never acked it. */
    ctl->move_doc_gen = 4;
    read_set("D", 4);
    ui_clear();                                   /* flag lost, no ack */
    read_set("D", 4);
    CHECK(ctl->set_doc_gen != 4, "an unacked set is never aligned as 'the same set'");

    /* Move RELOADING the same set, after it was acked: aligns in C, no UI switch. */
    shadow_set_pages_ack_aligned(shadow_set_pages_published_gen());
    ctl->move_doc_gen = 5;
    read_set("D", 5);
    CHECK(ctl->set_doc_gen == 5 && !raised(), "same-set reload aligns quietly (%u)", ctl->set_doc_gen);

    /* A READ YOUNGER THAN 300 ms, or one the model's generation has since
     * moved past, is not consumed. */
    m_gen = 6;
    CHECK(!shadow_set_pages_consume_read("E", "E", 6, 1, 50), "a 50 ms read waits");
    CHECK(!shadow_set_pages_consume_read("E", "E", 5, 1, 1000), "a read from the old generation is dropped");
    CHECK(shadow_set_pages_consume_read("E", "E", 6, 1, 1000), "the settled read lands");

    /* No model: every read is consumed, generations are all 0 (pre-model). */
    m_active = 0;
    CHECK(shadow_set_pages_consume_read("F", "F", 0, 0, 0), "no model: consumed at once");

    if (fails) { printf("test_set_alignment: %d FAILED\n", fails); return 1; }
    printf("test_set_alignment: PASS\n");
    return 0;
}
