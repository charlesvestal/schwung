/* drum_lanes.c: a Move drum track's pads as 16 monophonic pitched lanes, sent
 * as MPE -- channel per pad, the pad's own note, pitch as per-note bend, CC 3
 * marking a 16 Pitches note. Drives the sequenced and live paths and checks
 * the MIDI they emit. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "drum_lanes.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { int block; uint8_t st, d1, d2; } ev_t;
static ev_t ev[8192];
static int nev, cur_block;
static void sink(void *ctx, uint8_t st, uint8_t d1, uint8_t d2)
{
    (void)ctx;
    if (nev < (int)(sizeof ev / sizeof ev[0])) ev[nev++] = (ev_t){ cur_block, st, d1, d2 };
}
static void clear(void) { nev = 0; cur_block = 0; }

static int count(uint8_t st, int d1)
{
    int c = 0;
    for (int i = 0; i < nev; i++) if (ev[i].st == st && (d1 < 0 || ev[i].d1 == d1)) c++;
    return c;
}
static int first_block(uint8_t st, int d1)
{
    for (int i = 0; i < nev; i++) if (ev[i].st == st && (d1 < 0 || ev[i].d1 == d1)) return ev[i].block;
    return -1;
}
/* The bend sent on `lane` just before its n-th NoteOn, in semitones (NAN if none). */
static double bend_before_on(int lane, int nth)
{
    int seen = 0;
    for (int i = 0; i < nev; i++) {
        if (ev[i].st != (0x90 | lane)) continue;
        if (seen++ != nth) continue;
        for (int j = i - 1; j >= 0; j--)
            if (ev[j].st == (0xE0 | lane)) return ((ev[j].d1 | (ev[j].d2 << 7)) - 8192) * 48.0 / 8191.0;
        return NAN;
    }
    return NAN;
}
static int cc3_before_on(int lane, int nth)
{
    int seen = 0;
    for (int i = 0; i < nev; i++) {
        if (ev[i].st != (0x90 | lane)) continue;
        if (seen++ != nth) continue;
        for (int j = i - 1; j >= 0; j--)
            if (ev[j].st == (0xB0 | lane) && ev[j].d1 == DL_CC_PITCHED) return ev[j].d2;
        return -1;
    }
    return -1;
}

/* ---- the MIDI shape ---------------------------------------------------- */

static void test_emit_shape(void)
{
    clear();
    dl_emit_on(sink, NULL, 4, 90, 1, 13.0);
    CHECK(nev == 3);
    CHECK(ev[0].st == 0xB4 && ev[0].d1 == 3 && ev[0].d2 == 1);   /* CC 3 = pitched, first */
    CHECK(ev[1].st == 0xE4);                                       /* then the bend */
    CHECK(fabs(bend_before_on(4, 0) - 13.0) < 0.01);
    CHECK(ev[2].st == 0x94 && ev[2].d1 == 40 && ev[2].d2 == 90);   /* the PAD's note, never moved */
    clear();
    dl_emit_on(sink, NULL, 0, 100, 0, 0.0);                        /* a plain hit */
    CHECK(ev[0].d2 == 0 && ev[1].d1 == 0 && ev[1].d2 == 64);       /* CC 3 = 0, bend centred */
    CHECK(ev[2].st == 0x90 && ev[2].d1 == 36);
    clear();
    dl_emit_on(sink, NULL, 2, 100, 1, 60.0);                       /* past +48: clamped, not wrapped */
    CHECK(ev[1].d1 == 0x7F && ev[1].d2 == 0x7F);
    clear();
    dl_emit_off(sink, NULL, 15);
    CHECK(nev == 1 && ev[0].st == 0x8F && ev[0].d1 == 51);
}

/* ---- sequenced ---------------------------------------------------------- */

static mm_play_clip_t clip;
static void add(int pitch, double start, double dur, float off, float vel)
{
    clip.note[clip.n++] = (mm_play_note_t){ .pitch = pitch, .pitch_offset = off, .has_pitch = 1,
                                             .vel = vel, .start = start, .dur = dur };
}
static void reset_clip(uint64_t id)
{
    memset(&clip, 0, sizeof clip);
    clip.valid = 1;
    clip.clip_id = id;
}
static void run(dl_track_t *st, dl_window_t w, double pos, double blk, int nblocks)
{
    for (int b = 0; b < nblocks; b++) {
        cur_block = b;
        double p = pos + b * blk;
        if (w.loop && p >= w.le) p = w.ls + fmod(p - w.ls, w.le - w.ls);
        dl_block(st, &clip, w, p, blk, sink, NULL);
    }
}
#define BLK (1.0 / 64.0)
#define W4 ((dl_window_t){ 0.0, 4.0, 1 })
#define W8 ((dl_window_t){ 0.0, 8.0, 1 })

static void test_lane_channel_and_pitch(void)
{
    reset_clip(7);
    add(36, 0.0, 0.25, 0.0f, 100); clip.note[0].has_pitch = 0;    /* plain */
    add(40, 0.5, 0.25, 13.0f, 90);                                 /* 16 Pitches +13 */
    add(51, 1.0, 0.25, -2.0f, 127);                                /* last lane, -2 */
    add(52, 1.5, 0.25, 0.0f, 100);                                 /* not a drum cell: ignored */
    dl_track_t st; memset(&st, 0, sizeof st); clear();
    run(&st, W4, 0.0, BLK, 4 * 64);
    CHECK(count(0x90, 36) == 1 && cc3_before_on(0, 0) == 0 && fabs(bend_before_on(0, 0)) < 0.01);
    CHECK(count(0x94, 40) == 1 && cc3_before_on(4, 0) == 1 && fabs(bend_before_on(4, 0) - 13.0) < 0.01);
    CHECK(count(0x9F, 51) == 1 && fabs(bend_before_on(15, 0) + 2.0) < 0.01);
    CHECK(count(0x90, -1) + count(0x94, -1) + count(0x9F, -1) == 3);
    CHECK(first_block(0x94, 40) == 32);
    CHECK(count(0x80, 36) == 1 && count(0x84, 40) == 1 && count(0x8F, 51) == 1);
    CHECK(first_block(0x80, 36) == 16);
}

static void test_monophonic_lane(void)
{
    reset_clip(8);
    add(38, 0.0, 1.0, 0.0f, 100);
    add(38, 0.5, 0.25, 5.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear();
    run(&st, W4, 0.0, BLK, 2 * 64);
    CHECK(count(0x92, 38) == 2 && count(0x82, 38) == 2);
    CHECK(first_block(0x82, 38) == 32);                            /* the first is cut at 0.5 */
    CHECK(fabs(bend_before_on(2, 1) - 5.0) < 0.01);
    int off_i = -1, on2 = -1, seen = 0;
    for (int i = 0; i < nev; i++) {
        if (ev[i].st == 0x82 && off_i < 0) off_i = i;
        if (ev[i].st == 0x92 && seen++ == 1) on2 = i;
    }
    CHECK(off_i >= 0 && on2 > off_i);                              /* the cut comes first */
}

static void test_loop_wrap(void)
{
    reset_clip(9);
    add(36, 0.0, 0.1, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear();
    for (int b = 0; b < 10; b++) {
        cur_block = b;
        dl_block(&st, &clip, (dl_window_t){ 0.0, 1.0, 1 }, fmod(b * 0.3, 1.0), 0.3, sink, NULL);
    }
    CHECK(count(0x90, 36) == 3);
}

static void test_wrapped_note_length(void)
{
    reset_clip(15);
    add(36, 0.0, 0.25, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear();
    const double starts[3] = { 0.9, 0.2, 0.5 };
    for (int b = 0; b < 3; b++) {
        cur_block = b;
        dl_block(&st, &clip, (dl_window_t){ 0.0, 1.0, 1 }, starts[b], 0.3, sink, NULL);
    }
    CHECK(first_block(0x90, 36) == 0 && first_block(0x80, 36) == 2);
}

static void test_off_counts_the_offset_in_block(void)
{
    reset_clip(14);
    add(36, 0.05, 0.1, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear();
    for (int b = 0; b < 4; b++) {
        cur_block = b;
        dl_block(&st, &clip, W4, b * 0.1, 0.1, sink, NULL);
    }
    CHECK(first_block(0x90, 36) == 0 && first_block(0x80, 36) == 2);
}

static void test_stop_and_clip_change_release(void)
{
    reset_clip(10);
    add(36, 0.0, 4.0, 0.0f, 100);
    add(37, 0.0, 4.0, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear();
    run(&st, W8, 0.0, BLK, 4);
    CHECK(count(0x90, -1) == 1 && count(0x91, -1) == 1 && count(0x80, -1) == 0);
    cur_block = 99;
    dl_block(&st, &clip, W8, -1.0, BLK, sink, NULL);               /* stopped */
    CHECK(count(0x80, 36) == 1 && count(0x81, 37) == 1);
    clear();
    run(&st, W8, 0.0, BLK, 2);
    reset_clip(11);                                                /* another clip launched */
    add(40, 2.0, 1.0, 0.0f, 100);
    cur_block = 50;
    dl_block(&st, &clip, W8, 0.5, BLK, sink, NULL);
    CHECK(count(0x80, 36) == 1 && count(0x81, 37) == 1);
}

static void test_unknown_notes_are_silence(void)
{
    reset_clip(12);
    add(36, 0.0, 4.0, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear();
    run(&st, W8, 0.0, BLK, 2);
    clip.valid = 0;
    cur_block = 5;
    dl_block(&st, &clip, W8, 0.1, BLK, sink, NULL);
    CHECK(count(0x80, 36) == 1);
    clear();
    run(&st, W8, 0.0, BLK, 2);
    CHECK(nev == 0);
}

static void test_min_one_block(void)
{
    reset_clip(13);
    add(36, 0.0, 0.001, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear();
    run(&st, W4, 0.0, BLK, 3);
    CHECK(first_block(0x90, 36) == 0 && first_block(0x80, 36) == 1);
}

/* ---- live ---------------------------------------------------------------- */

static mm_live_rec_t R(int kind, int a, float b, int64_t id)
{
    return (mm_live_rec_t){ 0.0, MM_LIVE_EP_INPUT, a, b, id, (uint32_t)kind };
}
static dl_live_t primed(void) { dl_live_t s; memset(&s, 0, sizeof s); s.primed = 1; return s; }
#define PB(semis) ((float)((semis) * (8191.0 / 48.0)))

static void test_live_decode_captured(void)
{
    /* Three records read out of Move's EventBuffer on hardware (2026-09-28). */
    static const char *hex =
        "0000000000c05d407f000000804040402600000000000000261200000000000001000000" "02c04040"
        "0000000000c05f407f000000404040402600000000009442261200000000000000000000" "80408040"
        "0000000000c05f407f000000804040fefeffffff00f8ff44261200000000000003000000" "40404080";
    uint8_t raw[120];
    for (int i = 0; i < 120; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); raw[i] = (uint8_t)v; }
    mm_live_rec_t r[3];
    mm_decode_live_recs(raw, 3, r);
    CHECK(r[0].kind == MM_LIVE_KIND_OFF && r[0].a == 38 && r[0].id == 4646 && r[0].ep == 127);
    CHECK(r[1].kind == MM_LIVE_KIND_ON && r[1].b == 74.0f && r[1].id == 4646);
    CHECK(r[2].kind == MM_LIVE_KIND_PNCC && r[2].a == -2 && fabs(r[2].b / (8191.0 / 48.0) - 12.0) < 0.01);
}

static void test_live_on_pitch_off(void)
{
    dl_live_t st = primed(); clear();
    mm_live_rec_t a[] = { R(0, 38, 74, 100), R(3, -2, PB(13), 100) };
    dl_live_ingest(&st, a, 2, 1, sink, NULL);
    CHECK(count(0x92, 38) == 1 && cc3_before_on(2, 0) == 1 && fabs(bend_before_on(2, 0) - 13.0) < 0.01);
    dl_live_ingest(&st, a, 2, 1, sink, NULL);                      /* the same records again */
    CHECK(count(0x92, 38) == 1);
    mm_live_rec_t b[] = { R(1, 38, 0, 100), R(0, 38, 74, 100), R(3, -2, PB(13), 100) };
    dl_live_ingest(&st, b, 3, 1, sink, NULL);
    CHECK(count(0x82, 38) == 1);
}

static void test_live_plain_vs_pitched_zero(void)
{
    dl_live_t st = primed(); clear();
    mm_live_rec_t p[] = { R(0, 36, 90, 200) };                     /* no PNCC: plain */
    dl_live_ingest(&st, p, 1, 1, sink, NULL);
    mm_live_rec_t z[] = { R(0, 37, 90, 201), R(3, -2, 0.0f, 201) }; /* 16 Pitches at +0 */
    dl_live_ingest(&st, z, 2, 1, sink, NULL);
    CHECK(cc3_before_on(0, 0) == 0 && cc3_before_on(1, 0) == 1);   /* told apart by CC 3 */
    CHECK(fabs(bend_before_on(0, 0)) < 0.01 && fabs(bend_before_on(1, 0)) < 0.01);
}

static void test_live_move_mono_cut(void)
{
    dl_live_t st = primed(); clear();
    mm_live_rec_t r[] = { R(0, 38, 68, 4712), R(3, -2, PB(6), 4712), R(1, 38, 0, 4712),
                          R(0, 38, 55, 4713), R(3, -2, PB(8), 4713) };
    dl_live_ingest(&st, r, 5, 1, sink, NULL);
    CHECK(count(0x92, 38) == 2 && count(0x82, 38) == 1);
    CHECK(fabs(bend_before_on(2, 0) - 6.0) < 0.01 && fabs(bend_before_on(2, 1) - 8.0) < 0.01);
}

static void test_live_lost_off_cannot_stick(void)
{
    dl_live_t st = primed(); clear();
    mm_live_rec_t a[] = { R(0, 40, 90, 800) };
    dl_live_ingest(&st, a, 1, 1, sink, NULL);
    mm_live_rec_t b[] = { R(0, 40, 90, 801), R(3, -2, PB(5), 801) };
    dl_live_ingest(&st, b, 2, 1, sink, NULL);
    CHECK(count(0x84, 40) == 1 && count(0x94, 40) == 2);           /* the lost off is supplied */
    int off_i = -1, on2 = -1, seen = 0;
    for (int i = 0; i < nev; i++) {
        if (ev[i].st == 0x84 && off_i < 0) off_i = i;
        if (ev[i].st == 0x94 && seen++ == 1) on2 = i;
    }
    CHECK(off_i >= 0 && on2 > off_i);
}

static void test_live_unrouted_still_advances(void)
{
    dl_live_t st = primed(); clear();
    mm_live_rec_t r[] = { R(0, 36, 90, 500) };
    dl_live_ingest(&st, r, 1, 0, sink, NULL);
    dl_live_ingest(&st, r, 1, 1, sink, NULL);
    CHECK(nev == 0);
}

static void test_live_off_after_track_switch(void)
{
    /* Started while routed; the Off arrives after the selection moved away. */
    dl_live_t st = primed(); clear();
    mm_live_rec_t on[] = { R(0, 36, 90, 600) };
    dl_live_ingest(&st, on, 1, 1, sink, NULL);
    mm_live_rec_t off[] = { R(1, 36, 0, 600) };
    dl_live_ingest(&st, off, 1, 0, sink, NULL);
    CHECK(count(0x80, 36) == 1);
}

static void test_live_id_reset(void)
{
    dl_live_t st = primed(); clear();
    st.last_on = 900000;
    mm_live_rec_t r[] = { R(0, 36, 90, 5) };
    dl_live_ingest(&st, r, 1, 1, sink, NULL);
    CHECK(count(0x90, 36) == 1);
}

static void test_live_ignores_other_endpoints(void)
{
    dl_live_t st = primed(); clear();
    mm_live_rec_t r[] = { R(0, 36, 90, 700) };
    r[0].ep = 1;
    dl_live_ingest(&st, r, 1, 1, sink, NULL);
    CHECK(nev == 0);
}

static void test_live_first_read_primes(void)
{
    dl_live_t st; memset(&st, 0, sizeof st); clear();
    mm_live_rec_t old[] = { R(0, 36, 90, 900), R(0, 37, 90, 901) };
    dl_live_ingest(&st, old, 2, 1, sink, NULL);
    CHECK(nev == 0 && st.last_on == 901);
    mm_live_rec_t fresh[] = { R(0, 36, 90, 900), R(0, 37, 90, 901), R(0, 38, 90, 902) };
    dl_live_ingest(&st, fresh, 3, 1, sink, NULL);
    CHECK(count(0x92, 38) == 1 && count(0x90, -1) == 0);
}

/* ---- the decoder, and the config ---------------------------------------- */

static void test_decoder_has_pitch(void)
{
    /* A plain note 40 @1.25 (id 11) and a 16 Pitches note 40 @2.0 at +1 (id 13),
     * in the record layout measured on hardware. */
    static const char *hex =
        "000000283ff40000000000003fd000000000000042fe00000000000001000000000000000b"
        "0000002840000000000000003fd000000000000042fe0000000000000100000001fffffffe000000010000000000000000406554aaaaaaaaab0000000d";
    uint8_t raw[128];
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); raw[i] = (uint8_t)v; }
    mm_note_t out[4];
    CHECK(mm_decode_notes_buf(raw, n, out, 4, NULL, 0) == 2);
    CHECK(out[0].pitch == 40 && !out[0].has_pitch && out[0].id == 11);
    CHECK(out[1].pitch == 40 && out[1].has_pitch && fabs(out[1].pitch_offset - 1.0) < 0.01);
}

static void test_config_parse(void)
{
    int on[MM_TRACKS];
    dl_config_parse("1\n3 old fields are ignored\n9\n# comment\n", on);
    CHECK(on[0] == 1 && on[1] == 0 && on[2] == 1 && on[3] == 0);
    dl_config_parse("", on);
    CHECK(!on[0] && !on[1] && !on[2] && !on[3]);
}

int main(void)
{
    test_emit_shape();
    test_lane_channel_and_pitch();
    test_monophonic_lane();
    test_loop_wrap();
    test_wrapped_note_length();
    test_off_counts_the_offset_in_block();
    test_stop_and_clip_change_release();
    test_unknown_notes_are_silence();
    test_min_one_block();
    test_live_decode_captured();
    test_live_on_pitch_off();
    test_live_plain_vs_pitched_zero();
    test_live_move_mono_cut();
    test_live_lost_off_cannot_stick();
    test_live_unrouted_still_advances();
    test_live_off_after_track_switch();
    test_live_id_reset();
    test_live_ignores_other_endpoints();
    test_live_first_read_primes();
    test_decoder_has_pitch();
    test_config_parse();
    printf("%s test_drum_lanes (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
