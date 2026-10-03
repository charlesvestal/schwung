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

/* A KeyRandom is 20 random bytes, so its third word carries 4 bytes of
 * stale PADDING above them -- often the high half of an old pointer. On
 * hardware one landed inside the image range (0x55706e6073), the walker took
 * it for the wrapper's vptr, read the real vptr as the element, refused, and
 * every walk of that container failed until the next set load: the model
 * stalled with mute/solo follow, lanes and Undo off. Each set load mints new
 * keys, so it struck every few loads. */
static void test_tree_key_looks_like_vptr(void)
{
    memset(mem, 0, sizeof mem);
    uint64_t hdr = FAKE_BASE + 0x40;
    uint64_t A = FAKE_BASE + 0x8000, B = FAKE_BASE + 0x9000, C = FAKE_BASE + 0xa000;
    make_node(0, 0, 0, node_at(1), 0, A);
    make_node(1, node_at(0), node_at(2), hdr + 8, 0, B);
    make_node(2, 0, 0, node_at(1), 0, C);
    put(node_at(1) + 0x38, IMG_LO + 0x6e6073);   /* the random key's tail, in the image */
    put(hdr, node_at(0)); put(hdr + 8, node_at(1)); put(hdr + 16, 3);
    uint64_t out[8];
    int n = mm_tree_elems(fake_read, NULL, hdr, IMG_LO, IMG_HI, out, 8);
    CHECK(n == 3 && out[0] == A && out[1] == B && out[2] == C);
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

/* Move's notes buffer. The RECORDING: track 1 of "Lane Test" (Gen Purpose
 * Kit), 2026-09-28 -- 16 Pitches notes (a pitch lane), a 16 Pitches note
 * played with pressure (pitch + pressure lanes), two pressure-only notes, as
 * dumped from MoveOriginal's memory. 1853 bytes. */
static const char *LANE_TEST_T1 =
    "0000002400000000000000003fd000000000000042fe0000000000000100000001fffffffe0000000100000000000000"
    "00406554aaa000000000000001000000243fe80000000000003fd000000000000042fe0000000000000100000001ffff"
    "fffe000000010000000000000000408fff000000000000000002000000243ff40000000000003fd000000000000042fe"
    "0000000000000100000001fffffffe00000001000000000000000040a3ff600000000000000003000000290000000000"
    "0000003fd000000000000042fe0000000000000100000001fffffffe000000010000000000000000406554aaa0000000"
    "00000005000000293fd00000000000003fd000000000000042fe0000000000000100000001fffffffe00000001000000"
    "0000000000408aa9d54000000000000004000000293ff00000000000003fd000000000000042fe000000000000010000"
    "0001fffffffe0000000100000000000000004097ff4000000000000000060000002e3fc6c174174174174003ae74dbaa"
    "d1f342fe0000000000000100000002fffffffe000000010000000000000000409aa9d540000000ffffffff0000001b3f"
    "e2acbacbacbacc00000000000000003fe481d162e7a2d700000000000000003fe481d162e7a2d7402e0000000000003f"
    "e6574545c8b9a3402e0000000000003fe6574545c8b9a3403e0000000000003fe82c5c68af4523403e0000000000003f"
    "e82c5c68af452340468000000000003fea0172ffea2d2e40468000000000003fea0172ffea2d2e404e0000000000003f"
    "ebd6e6e2cb43fa404e0000000000003febd6e6e2cb43fa4052c000000000003fedabfd7a062c064052c000000000003f"
    "edabfd7a062c0640568000000000003fef81715ce742d140568000000000003fef81715ce742d1405a4000000000003f"
    "f0ab443fe6e729405a4000000000003ff0ab443fe6e729405e0000000000003ff195fdeb81a0d4405e0000000000003f"
    "f195fdeb81a0d4405fc000000000003ff715cde88170d1405fc000000000003ff715cde88170d1405f0000000000003f"
    "f80059341ee4d7405f0000000000003ff80059341ee4d7405f8000000000003ff8eb13258f703d405f8000000000003f"
    "f8eb13258f703d405fc0000000000040035559041be4a7405fc0000000000040035559041be4a7405e40000000000000"
    "00000700000033be9175746e8cba403ff6822adcdc596842fe0000000000000100000001ffffffff0000001900000000"
    "00000000405fc000000000003fd4303531dec0d5405fc000000000003fd4303531dec0d5405c4000000000003fd7db1c"
    "f7a0ee6c405c4000000000003fd7db1cf7a0ee6c40574000000000003fdb854a2616be8340574000000000003fdb854a"
    "2616be8340544000000000003fdf3031ebd8ec1a40544000000000003fdf3031ebd8ec1a4051c000000000003fe16d2f"
    "8d275e194051c000000000003fe16d2f8d275e19404f8000000000003fe342a3700874e4404f8000000000003fe342a3"
    "700874e4404c8000000000003fe517ba07435cf0404c8000000000003fe517ba07435cf0404b8000000000003fe6ecd1"
    "2a29e870404b8000000000003fe6ecd12a29e870404c8000000000003fe8c2450d0aff3c404c8000000000003fe8c245"
    "0d0aff3c40504000000000003fea975ba445e74740504000000000003fea975ba445e74740560000000000003fec6ccf"
    "8726fe1340560000000000003fec6ccf8726fe13405f8000000000003fee41e61e61e61e405f8000000000003fee41e6"
    "1e61e61e405fc00000000000000000090000003340043b4ee379da863ff789627ee21cad42fe00004280000001000000"
    "01ffffffff0000001f3fe2ac5e0bb22f8000000000000000003fe481d162e7a2d700000000000000003fe481d162e7a2"
    "d740220000000000003fe656e885ce2e5740220000000000003fe656e885ce2e5740300000000000003fe82c5bdd03a1"
    "ae40300000000000003fe82c5bdd03a1ae40330000000000003fea0172ffea2d2e40330000000000003fea0172ffea2d"
    "2e40340000000000003febd6e6e2cb43fa40340000000000003febd6e6e2cb43fa40360000000000003fedabfd7a062c"
    "0640360000000000003fedabfd7a062c06403b0000000000003fef81715ce742d1403b0000000000003fef81715ce742"
    "d1403f0000000000003ff0ab43fa11156e403f0000000000003ff0ab43fa11156e40420000000000003ff195fdeb81a0"
    "d440420000000000003ff195fdeb81a0d440438000000000003ff280897cf4e69440438000000000003ff280897cf4e6"
    "9440460000000000003ff36b43288fa04040460000000000003ff36b43288fa04040498000000000003ff455ceba02e6"
    "0040498000000000003ff455ceba02e60040510000000000003ff5405a05a05a0640510000000000003ff5405a05a05a"
    "0640568000000000003ff62b13f710e56b40568000000000003ff62b13f710e56b405cc000000000003ff7159f42ae59"
    "71405cc000000000003ff7159f42ae5971405fc0000000000000000008";

static size_t hexbytes(const char *hex, uint8_t *out)
{
    size_t n = 0;
    for (const char *p = hex; *p;) {
        if (*p == ' ') { p++; continue; }
        unsigned v; sscanf(p, "%2x", &v); out[n++] = (uint8_t)v; p += 2;
    }
    return n;
}

/* Move 2.1.1 recordings, read live off the device (2026-10-03): the same
 * records with an f32 probability (1.0) after the head. Four plain notes on
 * pads 36-39; four hits of one pad, three with a pressure lane (21/1/23). */
static const char *NOTES_211_PLAIN =
    "0000002400000000000000003fd000000000000042c8000000000000013f8000000000000000000001000000253ff000"
    "00000000003fd000000000000042c8000000000000013f80000000000000000000020000002640000000000000003fd0"
    "00000000000042c8000000000000013f80000000000000000000030000002740080000000000003fd000000000000042"
    "c8000000000000013f8000000000000000000004";
static const char *NOTES_211_PRESSURE =
    "00000024400e9ef257dfac863ff04559b2b270f942fe000000000000013f80000000000001ffffffff000000153fdff4"
    "3c5b0b822c00000000000000003fe18b5bfb912d7000000000000000003fe18b5bfb912d70402e0000000000003fe31c"
    "4a21b96168402e0000000000003fe31c4a21b96168403e0000000000003fe4ad87efc4cdc1403e0000000000003fe4ad"
    "87efc4cdc140468000000000003fe63e7615ed01b940468000000000003fe63e7615ed01b9404e0000000000003fe7cf"
    "b3e3f86e13404e0000000000003fe7cfb3e3f86e1340524000000000003fe960a20a20a20a40524000000000003fe960"
    "a20a20a20a40568000000000003feaf1dfd82c0e6440568000000000003feaf1dfd82c0e644059c000000000003fec82"
    "cdfe54425b4059c000000000003fec82cdfe54425b405bc000000000003fee140bcc5faeb5405bc000000000003fee14"
    "0bcc5faeb540584000000000003fefa4fa7e33862140584000000000003fefa4fa7e3386214041000000000000000000"
    "0100000024401f0d1073eff92e3fe317f46800ae9742da000000000000013f80000000000001ffffffff000000013fdf"
    "f43c5b0b822c00000000000000000000000200000024402792bdeec7a6a93ff15d3a7690233142a6000000000000013f"
    "80000000000001ffffffff000000173fdff43c5b0b822c00000000000000003fe18b0c53adf50e00000000000000003f"
    "e18b0c53adf50e40240000000000003fe31c4a21b9616840240000000000003fe31c4a21b9616840330000000000003f"
    "e4ad38d38d38d440330000000000003fe4ad38d38d38d4403c0000000000003fe63e7615ed01b9403c0000000000003f"
    "e63e7615ed01b940428000000000003fe7cf64c7c0d92540428000000000003fe7cf64c7c0d92540460000000000003f"
    "e960a295cc457f40460000000000003fe960a295cc457f404a8000000000003feaf190bbf47976404a8000000000003f"
    "eaf190bbf4797640500000000000003fec82ce89ffe5d040500000000000003fec82ce89ffe5d04051c000000000003f"
    "ee13bcb02819c74051c000000000003fee13bcb02819c740514000000000003fefa4ab61fbf13340514000000000003f"
    "efa4ab61fbf133404c8000000000003ff09af4522ddd0c404c8000000000003ff09af4522ddd0c403200000000000000"
    "00000300000024402b9aed4bec04923fc4c333790904ed41d0000000000000013f8000000000000000000004";

static const mm_note_t *by_id(const mm_note_t *nt, int n, int64_t id)
{
    for (int i = 0; i < n; i++) if (nt[i].id == id) return &nt[i];
    return NULL;
}

static void test_notes_lanes(void)
{
    static uint8_t buf[4096], bad[4096];
    static mm_note_t nt[64];
    static mm_expr_point_t pool[256];
    const size_t len = hexbytes(LANE_TEST_T1, buf);
    CHECK(len == 1853);

    /* The whole recording, every record, exactly to the end. */
    const int n = mm_decode_notes_buf(buf, len, MM_NOTES_FMT_210, nt, 64, pool, 256);
    CHECK(n == 9);
    /* 16 Pitches: the pad's own note, the pitch only in the lane -- exact
     * semitones at 8191/48 per semitone. */
    const struct { int64_t id; int pitch; double semis; int press; } want[] = {
        { 1, 36, 1, 0 }, { 2, 36, 6, 0 }, { 3, 36, 15, 0 }, { 5, 41, 1, 0 }, { 4, 41, 5, 0 }, { 6, 41, 9, 0 },
        { 7, 46, 10, 27 },                     /* pitch + pressure */
        { 9, 51, 0, 25 }, { 8, 51, 0, 31 },    /* pressure only */
    };
    int pts = 0;
    for (unsigned k = 0; k < sizeof want / sizeof want[0]; k++) {
        const mm_note_t *x = by_id(nt, n, want[k].id);
        CHECK(x != NULL);
        if (!x) continue;
        CHECK(x->pitch == want[k].pitch);
        CHECK(fabs(x->pitch_offset - want[k].semis) < 1e-3);
        CHECK(x->pressure_count == want[k].press);
        pts += x->pressure_count;
    }
    /* The pressure lane is exposed, as stored: step pairs, beats from the note. */
    const mm_note_t *n9 = by_id(nt, n, 9);
    CHECK(n9 && n9->pressure_first >= 0 && pool[n9->pressure_first].time == 0.0 &&
          pool[n9->pressure_first].value == 127.0);
    const mm_note_t *n8 = by_id(nt, n, 8);
    CHECK(n8 && fabs(pool[n8->pressure_first + 2].value - 9.0) < 1e-9 &&
          pool[n8->pressure_first + 1].time == pool[n8->pressure_first + 2].time);   /* a step */
    CHECK(pts == 83);

    /* The shape from the FIRST capture (a plain note and a one-point pitch
     * lane) decodes the same way: the "00000001 fffffffe" once taken for a
     * marker is lane_count 1, type -2. */
    {
        static const char *mixed =
            "00000028 3ff4000000000000 3fd0000000000000 42fe0000 00000000 01 000000000000000b"
            "00000028 3ffc000000000000 3fd0000000000000 42fe0000 00000000 01 00000001 fffffffe 00000001 0000000000000000 40a554aaa0000000 0000000c"
            "00000028 4000000000000000 3fd0000000000000 42fe0000 00000000 01 00000001 fffffffe 00000001 0000000000000000 406554aaa0000000 0000000d";
        uint8_t mb[256];
        const size_t ml = hexbytes(mixed, mb);
        CHECK(mm_decode_notes_buf(mb, ml, MM_NOTES_FMT_210, nt, 8, NULL, 0) == 3);
        CHECK(nt[0].id == 0xb && nt[1].id == 12 && nt[2].id == 13);
        CHECK(fabs(nt[1].pitch_offset - 16.0) < 1e-6 && fabs(nt[2].pitch_offset - 1.0) < 1e-6);
        CHECK(nt[0].pitch_offset == 0.0 && nt[0].pressure_count == 0);
    }

    /* ALL OR NOTHING: every truncation of the recording is unknown (-1),
     * except the ones that land exactly on a record boundary. */
    int boundaries = 0, bad_trunc = 0;
    for (size_t cut = 1; cut < len; cut++) {
        int r = mm_decode_notes_buf(buf, cut, MM_NOTES_FMT_210, nt, 64, pool, 256);
        if (r >= 0) boundaries++;
        else if (r != -1) bad_trunc++;
    }
    CHECK(bad_trunc == 0);
    CHECK(boundaries == n - 1);                         /* one clean prefix per record end */

    /* An unknown lane type (-3) is unknown, not a guess. */
    memcpy(bad, buf, len);
    {   /* note id 1's lane type sits at 29 + 4 */
        bad[33] = 0xff; bad[34] = 0xff; bad[35] = 0xff; bad[36] = 0xfd;
        CHECK(mm_decode_notes_buf(bad, len, MM_NOTES_FMT_210, nt, 64, pool, 256) == -1);
    }
    /* A point count that runs past the end. */
    memcpy(bad, buf, len);
    bad[37] = 0x7f;
    CHECK(mm_decode_notes_buf(bad, len, MM_NOTES_FMT_210, nt, 64, pool, 256) == -1);
    /* An absurd lane count. */
    memcpy(bad, buf, len);
    bad[29] = 0x00; bad[30] = 0x00; bad[31] = 0x10; bad[32] = 0x00;
    CHECK(mm_decode_notes_buf(bad, len, MM_NOTES_FMT_210, nt, 64, pool, 256) == -1);
    /* Too little room: never a partial list. */
    CHECK(mm_decode_notes_buf(buf, len, MM_NOTES_FMT_210, nt, 8, pool, 256) == -1);
    CHECK(mm_decode_notes_buf(buf, len, MM_NOTES_FMT_210, nt, 64, pool, 50) == -1);
    /* ...but no pool at all is fine: counted, not kept. */
    CHECK(mm_decode_notes_buf(buf, len, MM_NOTES_FMT_210, nt, 64, NULL, 0) == 9);
    CHECK(mm_decode_notes_buf(buf, 0, MM_NOTES_FMT_210, nt, 64, pool, 256) == 0);   /* an empty clip is not unknown */
}

static void test_notes_probability_layout(void)
{
    static uint8_t buf[2048], t1[4096];
    static mm_note_t nt[16];
    static mm_expr_point_t pool[128];

    size_t len = hexbytes(NOTES_211_PLAIN, buf);
    CHECK(len == 164);
    CHECK(mm_decode_notes_buf(buf, len, MM_NOTES_FMT_PROB, nt, 16, pool, 128) == 4);
    for (int i = 0; i < 4; i++) {
        CHECK(nt[i].pitch == 36 + i && nt[i].id == i + 1);
        CHECK(nt[i].start == (double)i && fabs(nt[i].vel - 100.0f) < 1e-4);
        CHECK(nt[i].probability == 1.0f && nt[i].enabled == 1);
        CHECK(nt[i].pressure_count == 0 && nt[i].pitch_offset == 0.0);
    }
    /* The wrong layout is UNKNOWN, never notes: this is the 2.1.1 bug --
     * the probability's 0x3f800000 read as a lane count. */
    CHECK(mm_decode_notes_buf(buf, len, MM_NOTES_FMT_210, nt, 16, pool, 128) == -1);
    /* A probability outside 0..1 is not a probability. */
    buf[29] = 0x40;                                   /* 2.0f */
    CHECK(mm_decode_notes_buf(buf, len, MM_NOTES_FMT_PROB, nt, 16, pool, 128) == -1);
    buf[29] = 0x3f; buf[30] = 0x00;                   /* 0.5f */
    CHECK(mm_decode_notes_buf(buf, len, MM_NOTES_FMT_PROB, nt, 16, pool, 128) == 4);
    CHECK(nt[0].probability == 0.5f);

    len = hexbytes(NOTES_211_PRESSURE, buf);
    CHECK(len == 908);
    CHECK(mm_decode_notes_buf(buf, len, MM_NOTES_FMT_PROB, nt, 16, pool, 128) == 4);
    const int press[4] = { 21, 1, 23, 0 };
    const float vel[4] = { 127, 109, 83, 26 };
    for (int i = 0; i < 4; i++) {
        CHECK(nt[i].pitch == 36 && nt[i].id == i + 1);
        CHECK(nt[i].pressure_count == press[i] && fabs(nt[i].vel - vel[i]) < 1e-3);
        CHECK(nt[i].probability == 1.0f);
    }
    CHECK(mm_decode_notes_buf(buf, len, MM_NOTES_FMT_210, nt, 16, pool, 128) == -1);
    /* ALL OR NOTHING holds in the new layout too. */
    int boundaries = 0;
    for (size_t cut = 1; cut < len; cut++) {
        int r = mm_decode_notes_buf(buf, cut, MM_NOTES_FMT_PROB, nt, 16, pool, 128);
        CHECK(r == -1 || r >= 0);
        if (r >= 0) boundaries++;
    }
    CHECK(boundaries == 3);

    /* Plausibility: a pitch Move cannot store is not a note. */
    len = hexbytes(NOTES_211_PLAIN, buf);
    buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 200;
    CHECK(mm_decode_notes_buf(buf, len, MM_NOTES_FMT_PROB, nt, 16, pool, 128) == -1);

    /* And a 2.1.0 recording is not readable as 2.1.1. */
    len = hexbytes(LANE_TEST_T1, t1);
    CHECK(mm_decode_notes_buf(t1, len, MM_NOTES_FMT_PROB, nt, 16, pool, 128) == -1);
    CHECK(mm_decode_notes_buf(t1, len, 7, nt, 16, pool, 128) == -1);   /* unknown layout */
}

/* A FIELD WE HAVE NEVER SEEN. 2.1.1 added one after the head; the next
 * firmware may add another. The layout is learned from the clip itself --
 * exactly one candidate may decode it, to the last byte -- so a new field
 * costs nothing, and anything ambiguous is refused rather than guessed. */
static size_t widen_211_plain(uint8_t *dst, int notes, uint32_t pre_word, int post_words)
{
    static uint8_t src[256];
    hexbytes(NOTES_211_PLAIN, src);
    size_t o = 0;
    for (int i = 0; i < notes; i++) {
        const uint8_t *r = src + 41u * (size_t)i;     /* a 2.1.1 plain record is 41 bytes */
        memcpy(dst + o, r, 33); o += 33;               /* head + probability */
        dst[o++] = (uint8_t)(pre_word >> 24); dst[o++] = (uint8_t)(pre_word >> 16);
        dst[o++] = (uint8_t)(pre_word >> 8);  dst[o++] = (uint8_t)pre_word;
        memcpy(dst + o, r + 33, 8); o += 8;            /* lane_count 0 + id */
        for (int k = 0; k < post_words; k++) { memset(dst + o, 0xAB, 4); o += 4; }
    }
    return o;
}

static void test_notes_unknown_layout(void)
{
    static uint8_t buf[4096];
    static mm_note_t nt[16];

    /* The known layouts detect as themselves. */
    size_t len = hexbytes(NOTES_211_PLAIN, buf);
    CHECK(mm_detect_note_layout(buf, len) == MM_NOTES_FMT_PROB);
    len = hexbytes(NOTES_211_PRESSURE, buf);
    CHECK(mm_detect_note_layout(buf, len) == MM_NOTES_FMT_PROB);
    len = hexbytes(LANE_TEST_T1, buf);
    CHECK(mm_detect_note_layout(buf, len) == MM_NOTES_FMT_210);

    /* One new field after probability: learned, and the notes are right. */
    len = widen_211_plain(buf, 4, 7, 0);
    CHECK(mm_decode_notes_buf(buf, len, MM_NOTES_FMT_PROB, nt, 16, NULL, 0) == -1);
    const int f = mm_detect_note_layout(buf, len);
    CHECK(f == MM_NOTES_FMT_UNKNOWN(2, 0));
    CHECK(mm_decode_notes_buf(buf, len, f, nt, 16, NULL, 0) == 4);
    CHECK(nt[3].pitch == 39 && nt[3].id == 4 && nt[3].start == 3.0);

    /* ...and one after the id. */
    len = widen_211_plain(buf, 4, 7, 1);
    CHECK(mm_detect_note_layout(buf, len) == MM_NOTES_FMT_UNKNOWN(2, 1));

    /* AMBIGUOUS is refused: one note whose new field is 0 fits both
     * "+2 words before the lanes" and "+1 before, +1 after the id". */
    len = widen_211_plain(buf, 1, 0, 0);
    CHECK(mm_detect_note_layout(buf, len) == -1);
    CHECK(mm_detect_note_layout(buf, 0) == -1);       /* an empty clip says nothing */
}

/* The reader's tear check must not treat a moving scalar as a torn read:
 * tempo moving under Link made every pair disagree and the model never
 * published again (hardware, 2026-09-29). Structure must still tear. */
static void test_pair_consistent(void)
{
    static move_model_t a, b;
    memset(&a, 0, sizeof a);
    a.valid = 1; a.tempo = 120.0; a.master_db = -6.0;
    a.track[0].volume = -2.0; a.track[0].pan = 0.1;
    a.track[0].slot[0].exists = 1; a.track[0].slot[0].clip_id = 42;
    b = a;
    CHECK(mm_pair_consistent(&a, &b) == 1);
    b.tempo = 120.0000001; b.song_beats = 3.5; b.master_db = -6.5;
    b.track[0].volume = -2.1; b.track[0].pan = 0.2;
    b.track[0].speaker_value = 0.5; b.track[0].solo_value = 0.5;
    CHECK(mm_pair_consistent(&a, &b) == 1);      /* values moved, shape did not */
    b.track[0].slot[0].clip_id = 43;
    CHECK(mm_pair_consistent(&a, &b) == 0);      /* a different clip IS a tear */
    b = a; b.track[0].muted = 1;
    CHECK(mm_pair_consistent(&a, &b) == 0);      /* derived flags stay compared */
    b = a; b.doc_id = 7;
    CHECK(mm_pair_consistent(&a, &b) == 0);      /* a set load mid-read is a tear */
}

int main(void)
{
    test_pair_consistent();
    test_notes_lanes();
    test_notes_probability_layout();
    test_notes_unknown_layout();
    test_history();
    test_resolution();
    test_stub();
    test_sso();
    test_tree(1);
    test_tree(0);
    test_tree_key_looks_like_vptr();
    test_position();
    if (fails) { printf("test_move_model: %d FAILED\n", fails); return 1; }
    printf("test_move_model: PASS\n");
    return 0;
}
