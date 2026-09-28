/* Pure pieces of move_model.c: the flip member-stub decoder, the libc++
 * string reader, the std::map walk over flip container nodes, and the clip
 * position arithmetic. The runtime half reads Move's own memory and is
 * verified on the device (move_model_on -> move_model.log). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "move_model.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* A fake address space: one buffer mapped at FAKE_BASE. */
#define FAKE_BASE 0x5500000000ull
static uint8_t mem[1 << 16];
static int fake_read(void *ctx, uint64_t a, void *buf, size_t n)
{
    (void)ctx;
    if (a < FAKE_BASE || a + n > FAKE_BASE + sizeof mem) return -1;
    memcpy(buf, mem + (a - FAKE_BASE), n);
    return 0;
}
static void put(uint64_t a, uint64_t v) { memcpy(mem + (a - FAKE_BASE), &v, 8); }

#define IMG_LO 0x5590000000ull
#define IMG_HI 0x5592000000ull

static void test_stub(void)
{
    /* Words captured from MoveOriginal 2.1.0. */
    uint32_t mov[4] = { 0x52826a08u, 0x8b080000u, 0xd65f03c0u, 0x52828e08u }; /* live.Song.mTracks */
    uint32_t add[4] = { 0x91028000u, 0xd65f03c0u, 0, 0 };                      /* add x0,x0,#0xa0 */
    uint32_t ret[4] = { 0xd65f03c0u, 0, 0, 0 };
    uint32_t bad[4] = { 0xa9bd7bfdu, 0, 0, 0 };                                /* stp: not a stub */
    uint32_t off = 0;
    CHECK(mm_decode_member_stub(mov, &off) == 0 && off == 0x1350);
    CHECK(mm_decode_member_stub(add, &off) == 0 && off == 0xa0);
    CHECK(mm_decode_member_stub(ret, &off) == 0 && off == 0);
    CHECK(mm_decode_member_stub(bad, &off) == -1);
    uint32_t lsl[4] = { 0x91400400u, 0xd65f03c0u, 0, 0 };                      /* add x0,x0,#1,lsl#12 */
    CHECK(mm_decode_member_stub(lsl, &off) == 0 && off == 0x1000);
}

static void test_sso(void)
{
    uint8_t raw[24] = {0};
    char out[64];
    raw[0] = 10 << 1;
    memcpy(raw + 1, "mTransport", 10);
    CHECK(mm_sso_string(fake_read, NULL, raw, out, sizeof out) == 0 && strcmp(out, "mTransport") == 0);
    const char *longname = "mSetIsArrangementRecordingEnabledMessage";
    memcpy(mem + 0x100, longname, strlen(longname));
    uint64_t capv = 48 | 1, size = strlen(longname), ptr = FAKE_BASE + 0x100;
    memcpy(raw, &capv, 8); memcpy(raw + 8, &size, 8); memcpy(raw + 16, &ptr, 8);
    CHECK(mm_sso_string(fake_read, NULL, raw, out, sizeof out) == 0 && strcmp(out, longname) == 0);
    CHECK(mm_sso_string(fake_read, NULL, raw, out, 8) == -1);   /* too long for the buffer */
}

/* Build a 3-node tree   B(root) / A, C \  as a KeyFloat (Array) or KeyRandom
 * (Collection) map, and check the in-order walk returns A,B,C's elements. */
static uint64_t node_at(int k) { return FAKE_BASE + 0x1000 + (uint64_t)k * 0x100; }
static void make_node(int k, uint64_t l, uint64_t r, uint64_t p, int keyfloat, uint64_t elem)
{
    uint64_t n = node_at(k);
    put(n, l); put(n + 8, r); put(n + 16, p); put(n + 24, 1);
    put(n + 0x20, IMG_LO + 0x100);                              /* key vptr */
    if (keyfloat) {                                             /* {nbits, vector} */
        put(n + 0x28, 0x38); put(n + 0x30, FAKE_BASE + 0x800); put(n + 0x38, FAKE_BASE + 0x807);
        put(n + 0x40, FAKE_BASE + 0x808);
        put(n + 0x48, IMG_LO + 0x200); put(n + 0x50, elem);     /* wrapper {vptr, T*} */
    } else {                                                    /* 20 random bytes */
        put(n + 0x28, 0x76bd25fa16eacc28ull); put(n + 0x30, 0x845b4a61d6b3aa0cull);
        put(n + 0x38, 0xe47fb923ull);
        put(n + 0x40, IMG_LO + 0x200); put(n + 0x48, elem);
    }
}
static void test_tree(int keyfloat)
{
    memset(mem, 0, sizeof mem);
    uint64_t hdr = FAKE_BASE + 0x40;              /* {begin, root(=end.left), size} */
    uint64_t A = FAKE_BASE + 0x8000, B = FAKE_BASE + 0x9000, C = FAKE_BASE + 0xa000;
    make_node(0, 0, 0, node_at(1), keyfloat, A);
    make_node(1, node_at(0), node_at(2), hdr + 8, keyfloat, B);
    make_node(2, 0, 0, node_at(1), keyfloat, C);
    put(hdr, node_at(0)); put(hdr + 8, node_at(1)); put(hdr + 16, 3);
    uint64_t out[8];
    int n = mm_tree_elems(fake_read, NULL, hdr, IMG_LO, IMG_HI, out, 8);
    CHECK(n == 3 && out[0] == A && out[1] == B && out[2] == C);
    /* a size that disagrees with the walk is a TORN read, not a short list */
    put(hdr + 16, 4);
    CHECK(mm_tree_elems(fake_read, NULL, hdr, IMG_LO, IMG_HI, out, 8) == -1);
    /* empty map: begin == end */
    put(hdr, hdr + 8); put(hdr + 8, 0); put(hdr + 16, 0);
    CHECK(mm_tree_elems(fake_read, NULL, hdr, IMG_LO, IMG_HI, out, 8) == 0);
    /* a node pointer into unmapped memory fails instead of wandering */
    put(hdr, 0x6600000000ull); put(hdr + 16, 1);
    CHECK(mm_tree_elems(fake_read, NULL, hdr, IMG_LO, IMG_HI, out, 8) == -1);
}

static void test_position(void)
{
    mm_clip_t c = { .exists = 1, .region_start = 0, .region_end = 8, .loop_start = 0, .loop_end = 8, .loop_on = 1 };
    CHECK(fabs(mm_clip_position(&c, 16.0, 17.5) - 1.5) < 1e-9);    /* launched at 16 */
    CHECK(fabs(mm_clip_position(&c, 16.0, 26.0) - 2.0) < 1e-9);    /* wrapped once */
    /* start marker before a loop that begins later: plays in, then wraps in the loop */
    mm_clip_t d = { .exists = 1, .region_start = 0, .region_end = 12, .loop_start = 4, .loop_end = 12, .loop_on = 1 };
    CHECK(fabs(mm_clip_position(&d, 0, 3.0) - 3.0) < 1e-9);
    CHECK(fabs(mm_clip_position(&d, 0, 13.0) - 5.0) < 1e-9);
    mm_clip_t e = d; e.loop_on = 0;
    CHECK(mm_clip_position(&e, 0, 13.0) < 0);                         /* one-shot has ended */
    mm_clip_t z = { 0 };
    CHECK(mm_clip_position(&z, 0, 1.0) < 0);
}

static void test_resolution(void)
{
    /* Every name the 2.1.0 enum carries, from its flip::EnumClass. */
    const char *nm[] = { "1/8t", "1/16", "1/16t", "1/32", "1/32t", "1/64" };
    const double want[] = { 1.0 / 3, 0.25, 1.0 / 6, 0.125, 1.0 / 12, 0.0625 };
    const int trip[] = { 1, 0, 1, 0, 1, 0 };
    for (int k = 0; k < 6; k++) {
        double b = 0; uint8_t t = 9;
        CHECK(mm_parse_resolution(nm[k], &b, &t) == 0 && fabs(b - want[k]) < 1e-12 && t == trip[k]);
    }
    double b; uint8_t t;
    CHECK(mm_parse_resolution("Free", &b, &t) == -1);
}

/* flip's History<HistoryStoreMemory> laid out as libc++ does it, with three
 * transactions, walked through the undo/redo positions Move can be in. */
static void test_history(void)
{
    const uint64_t VH = IMG_LO + 0x10, VS = IMG_LO + 0x20, VT = IMG_LO + 0x30, VI = IMG_LO + 0x40;
    const uint64_t vh[] = { VH }, vs[] = { VS }, vt[] = { VT };
    mm_hist_vps_t v = { vh, 1, vs, 1, vt, 1 };
    const uint64_t obj = FAKE_BASE + 0x2000, sent = obj + 0x20;
    const uint64_t n1 = FAKE_BASE + 0x3000, n2 = FAKE_BASE + 0x3100, n3 = FAKE_BASE + 0x3200;
    memset(mem + 0x2000, 0, 0x1400);
    put(obj, VH); put(obj + 0x10, VS);
    /* sentinel <-> n1 <-> n2 <-> n3 <-> sentinel */
    put(sent, n3); put(sent + 8, n1); put(obj + 0x30, 3);
    const uint64_t nodes[3] = { n1, n2, n3 };
    for (int i = 0; i < 3; i++) {
        uint64_t nd = nodes[i];
        put(nd, i ? nodes[i - 1] : sent);
        put(nd + 8, i < 2 ? nodes[i + 1] : sent);
        put(nd + 0x10, VT); put(nd + 0x18, VI);
        put(nd + 0x30, 100 + (uint64_t)i);                 /* nbr_id */
    }
    move_model_t m;

    put(obj + 0x38, sent);                                 /* nothing undone */
    CHECK(mm_history_read(fake_read, NULL, obj, &v, &m) == 0 && m.hist_valid && m.hist_size == 3);
    CHECK(m.hist_undo_node == n3 && m.hist_undo_nbr == 102 && m.hist_redo_node == 0);

    put(obj + 0x38, n3);                                   /* one undo */
    CHECK(mm_history_read(fake_read, NULL, obj, &v, &m) == 0);
    CHECK(m.hist_undo_node == n2 && m.hist_undo_nbr == 101 && m.hist_redo_node == n3 && m.hist_redo_nbr == 102);

    put(obj + 0x38, n1);                                   /* everything undone */
    CHECK(mm_history_read(fake_read, NULL, obj, &v, &m) == 0);
    CHECK(m.hist_undo_node == 0 && m.hist_redo_node == n1);

    /* Empty. */
    put(sent, sent); put(sent + 8, sent); put(obj + 0x30, 0); put(obj + 0x38, sent);
    CHECK(mm_history_read(fake_read, NULL, obj, &v, &m) == 0 && m.hist_valid && !m.hist_undo_node && !m.hist_redo_node);

    /* Any vptr that is not the resolved one is NOT a history: invalid. */
    put(sent, n3); put(sent + 8, n1); put(obj + 0x30, 3); put(obj + 0x38, sent);
    put(n3 + 0x10, VT + 8);
    CHECK(mm_history_read(fake_read, NULL, obj, &v, &m) == -1 && !m.hist_valid);
    put(n3 + 0x10, VT);
    put(obj + 0x10, VS + 8);
    CHECK(mm_history_read(fake_read, NULL, obj, &v, &m) == -1 && !m.hist_valid);
    put(obj + 0x10, VS);
    CHECK(mm_history_read(fake_read, NULL, 0, &v, &m) == -1);
}

int main(void)
{
    test_history();
    test_resolution();
    test_stub();
    test_sso();
    test_tree(1);
    test_tree(0);
    test_position();
    if (fails) { printf("test_move_model: %d FAILED\n", fails); return 1; }
    printf("test_move_model: PASS\n");
    return 0;
}
