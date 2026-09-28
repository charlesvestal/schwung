/* drum_lanes.c: a Move drum clip played as 16 monophonic pitched lanes.
 * Drives dl_block block by block over a clip and checks the MIDI it emits. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "drum_lanes.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { int block; uint8_t st, d1, d2; } ev_t;
static ev_t ev[4096];
static int nev, cur_block;
static void sink(void *ctx, uint8_t st, uint8_t d1, uint8_t d2)
{
    (void)ctx;
    if (nev < (int)(sizeof ev / sizeof ev[0])) ev[nev++] = (ev_t){ cur_block, st, d1, d2 };
}

static mm_play_clip_t clip;
static void add(int pitch, double start, double dur, float off, float vel)
{
    clip.note[clip.n++] = (mm_play_note_t){ pitch, off, vel, start, dur };
}
static void reset_clip(uint64_t id)
{
    memset(&clip, 0, sizeof clip);
    clip.valid = 1;
    clip.clip_id = id;
}

/* Run `nblocks` from clip position `pos` (the transport is simply clip time
 * here, wrapped the way mm_clip_position does for a looping clip). */
static void run(dl_track_t *st, dl_window_t w, double pos, double blk, int nblocks)
{
    for (int b = 0; b < nblocks; b++) {
        cur_block = b;
        double p = pos + b * blk;
        if (w.loop && p >= w.le) p = w.ls + fmod(p - w.ls, w.le - w.ls);
        dl_block(st, &clip, w, p, blk, 60, sink, NULL);
    }
}

static int count(uint8_t st, int d1)
{
    int c = 0;
    for (int i = 0; i < nev; i++) if (ev[i].st == st && (d1 < 0 || ev[i].d1 == d1)) c++;
    return c;
}
static int first_block(uint8_t st, int d1)
{
    for (int i = 0; i < nev; i++) if (ev[i].st == st && ev[i].d1 == d1) return ev[i].block;
    return -1;
}

#define BLK (1.0 / 64.0)   /* a test block: 64 per beat, so note times land exactly */

static void test_lane_channel_and_pitch(void)
{
    /* Pad 36 plain, pad 40 in 16 Pitches (+13), pad 51 (last lane) at -2. */
    reset_clip(7);
    add(36, 0.0, 0.25, 0.0f, 100);
    add(40, 0.5, 0.25, 13.0f, 90);
    add(51, 1.0, 0.25, -2.0f, 127);
    add(52, 1.5, 0.25, 0.0f, 100);            /* not a drum cell: ignored */
    dl_track_t st; memset(&st, 0, sizeof st); nev = 0;
    run(&st, (dl_window_t){ 0.0, 4.0, 1 }, 0.0, BLK, 4 * 64);
    CHECK(count(0x90, 60) == 1);              /* lane 0 = channel 1, base note */
    CHECK(count(0x94, 73) == 1);              /* lane 4 = channel 5, 60 + 13 */
    CHECK(count(0x9F, 58) == 1);              /* lane 15 = channel 16, 60 - 2 */
    CHECK(count(0x90, -1) + count(0x94, -1) + count(0x9F, -1) == 3);
    CHECK(first_block(0x94, 73) == 32);       /* 0.5 beats = block 32 */
    /* every on has its off, on the same channel and note */
    CHECK(count(0x80, 60) == 1 && count(0x84, 73) == 1 && count(0x8F, 58) == 1);
    CHECK(first_block(0x80, 60) == 16);       /* 0.25 beats long -> off at block 16 */
}

static void test_monophonic_lane(void)
{
    /* Two overlapping notes on one pad: the second cuts the first. */
    reset_clip(8);
    add(38, 0.0, 1.0, 0.0f, 100);
    add(38, 0.5, 0.25, 5.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); nev = 0;
    run(&st, (dl_window_t){ 0.0, 4.0, 1 }, 0.0, BLK, 2 * 64);
    CHECK(count(0x82, 60) == 1 && first_block(0x82, 60) == 32);   /* cut at 0.5 */
    CHECK(count(0x92, 65) == 1 && count(0x82, 65) == 1);
    /* the cut comes BEFORE the new on in the same block */
    int off_i = -1, on_i = -1;
    for (int i = 0; i < nev; i++) {
        if (ev[i].st == 0x82 && ev[i].d1 == 60) off_i = i;
        if (ev[i].st == 0x92 && ev[i].d1 == 65) on_i = i;
    }
    CHECK(off_i >= 0 && on_i > off_i);
}

static void test_loop_wrap(void)
{
    /* A 1-beat loop with a note on its first step plays once per pass, and a
     * block that straddles the loop end starts it exactly once. */
    reset_clip(9);
    add(36, 0.0, 0.1, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); nev = 0;
    /* an awkward block that does not divide the loop: 0.3 beats */
    for (int b = 0; b < 10; b++) {
        cur_block = b;
        double p = fmod(b * 0.3, 1.0);
        dl_block(&st, &clip, (dl_window_t){ 0.0, 1.0, 1 }, p, 0.3, 60, sink, NULL);
    }
    /* 10 blocks x 0.3 = 3.0 beats = 3 passes; the first pass starts at 0 */
    CHECK(count(0x90, 60) == 3);
    CHECK(count(0x80, 60) == 3 || count(0x80, 60) == 2);   /* the last may still sound */
}

static void test_stop_and_clip_change_release(void)
{
    reset_clip(10);
    add(36, 0.0, 4.0, 0.0f, 100);
    add(37, 0.0, 4.0, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); nev = 0;
    run(&st, (dl_window_t){ 0.0, 8.0, 1 }, 0.0, BLK, 4);
    CHECK(count(0x90, -1) == 1 && count(0x91, -1) == 1 && count(0x80, -1) == 0);
    cur_block = 99;
    dl_block(&st, &clip, (dl_window_t){ 0.0, 8.0, 1 }, -1.0, BLK, 60, sink, NULL);   /* stopped */
    CHECK(count(0x80, 60) == 1 && count(0x81, 60) == 1);

    nev = 0;
    run(&st, (dl_window_t){ 0.0, 8.0, 1 }, 0.0, BLK, 2);
    reset_clip(11);                            /* the track launched another clip */
    add(40, 2.0, 1.0, 0.0f, 100);
    cur_block = 50;
    dl_block(&st, &clip, (dl_window_t){ 0.0, 8.0, 1 }, 0.5, BLK, 60, sink, NULL);
    CHECK(count(0x80, 60) == 1 && count(0x81, 60) == 1);   /* old clip released */
}

static void test_unknown_notes_are_silence(void)
{
    reset_clip(12);
    add(36, 0.0, 4.0, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); nev = 0;
    run(&st, (dl_window_t){ 0.0, 8.0, 1 }, 0.0, BLK, 2);
    clip.valid = 0;                            /* a blob that did not decode */
    cur_block = 5;
    dl_block(&st, &clip, (dl_window_t){ 0.0, 8.0, 1 }, 0.1, BLK, 60, sink, NULL);
    CHECK(count(0x80, 60) == 1);               /* released, not replayed */
    nev = 0;
    run(&st, (dl_window_t){ 0.0, 8.0, 1 }, 0.0, BLK, 2);
    CHECK(nev == 0);                           /* and nothing starts from it */
}

static void test_min_one_block(void)
{
    /* A note shorter than a block still gets its off in a LATER block. */
    reset_clip(13);
    add(36, 0.0, 0.001, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); nev = 0;
    run(&st, (dl_window_t){ 0.0, 4.0, 1 }, 0.0, BLK, 3);
    CHECK(first_block(0x90, 60) == 0 && first_block(0x80, 60) == 1);
}

static void test_off_counts_the_offset_in_block(void)
{
    /* Blocks of 0.1 beats. A note starting 0.05 into block 0, 0.1 long, ends at
     * 0.15 -- still sounding at block 1's start (0.1), so its off belongs to
     * block 2 (0.2). Measuring its length from the block start instead would
     * cut it at block 1, half a note early. */
    reset_clip(14);
    add(36, 0.05, 0.1, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); nev = 0;
    for (int b = 0; b < 4; b++) {
        cur_block = b;
        dl_block(&st, &clip, (dl_window_t){ 0.0, 4.0, 1 }, b * 0.1, 0.1, 60, sink, NULL);
    }
    CHECK(first_block(0x90, 60) == 0);
    CHECK(first_block(0x80, 60) == 2);
}

static void test_wrapped_note_length(void)
{
    /* A 1-beat loop, blocks of 0.3 starting at 0.9: the block wraps, and the
     * note on the loop's first beat starts 0.1 INTO it. 0.25 long, it ends
     * 0.35 after the block's start -- past block 1's start (0.3), so its off
     * belongs to block 2. Losing the wrap offset would cut it at block 1. */
    reset_clip(15);
    add(36, 0.0, 0.25, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); nev = 0;
    const double starts[3] = { 0.9, 0.2, 0.5 };
    for (int b = 0; b < 3; b++) {
        cur_block = b;
        dl_block(&st, &clip, (dl_window_t){ 0.0, 1.0, 1 }, starts[b], 0.3, 60, sink, NULL);
    }
    CHECK(first_block(0x90, 60) == 0);
    CHECK(first_block(0x80, 60) == 2);
}

static void test_config_parse(void)
{
    int slot[MM_TRACKS], base[MM_TRACKS];
    dl_config_parse("1 2\n3 4 48\n9 1\n2 7\n# comment\n", slot, base);
    CHECK(slot[0] == 1 && base[0] == 60);      /* track 1 -> slot 2 */
    CHECK(slot[2] == 3 && base[2] == 48);      /* track 3 -> slot 4, base 48 */
    CHECK(slot[1] == -1 && slot[3] == -1);     /* bad slot / absent: off */
    dl_config_parse("", slot, base);
    CHECK(slot[0] == -1 && slot[1] == -1 && slot[2] == -1 && slot[3] == -1);
}

/* ---- live ---------------------------------------------------------------- */

typedef struct { int slot; uint8_t st, d1, d2; } lev_t;
static lev_t lev[256];
static int nlev;
static void lsink(int slot, uint8_t st, uint8_t d1, uint8_t d2)
{
    if (nlev < 256) lev[nlev++] = (lev_t){ slot, st, d1, d2 };
}
static mm_live_rec_t R(int kind, int a, float b, int64_t id)
{
    return (mm_live_rec_t){ 0.0, MM_LIVE_EP_INPUT, a, b, id, (uint32_t)kind };
}

static void test_live_decode_captured(void)
{
    /* Three records read out of Move's EventBuffer on hardware (2026-09-28):
     * Off(38) id 4646, On(38, vel 74) id 4646, PerNoteCC(-2, 2047.9) id 4646. */
    static const char *hex =
        "0000000000c05d407f000000804040402600000000000000261200000000000001000000" "02c04040"
        "0000000000c05f407f000000404040402600000000009442261200000000000000000000" "80408040"
        "0000000000c05f407f000000804040fefeffffff00f8ff44261200000000000003000000" "40404080";
    uint8_t raw[120];
    for (int i = 0; i < 120; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); raw[i] = (uint8_t)v; }
    mm_live_rec_t r[3];
    mm_decode_live_recs(raw, 3, r);
    CHECK(r[0].kind == MM_LIVE_KIND_OFF && r[0].a == 38 && r[0].id == 4646 && r[0].ep == 127);
    CHECK(r[0].frame == 119.0);
    CHECK(r[1].kind == MM_LIVE_KIND_ON && r[1].a == 38 && r[1].b == 74.0f && r[1].id == 4646);
    CHECK(r[2].kind == MM_LIVE_KIND_PNCC && r[2].a == -2 && r[2].id == 4646);
    CHECK(fabs(r[2].b / (8191.0 / 48.0) - 12.0) < 0.01);
}

static void test_live_on_pitch_off(void)
{
    dl_live_t st; memset(&st, 0, sizeof st); st.primed = 1; nlev = 0;
    mm_live_rec_t a[] = { R(0, 38, 74, 100), R(3, -2, 13 * (8191.0 / 48.0), 100) };
    dl_live_ingest(&st, a, 2, 1, 60, lsink);
    CHECK(nlev == 1 && lev[0].slot == 1 && lev[0].st == 0x92 && lev[0].d1 == 73 && lev[0].d2 == 74);
    /* the same records again next block (they persist): nothing new */
    dl_live_ingest(&st, a, 2, 1, 60, lsink);
    CHECK(nlev == 1);
    mm_live_rec_t b[] = { R(1, 38, 0, 100), R(0, 38, 74, 100), R(3, -2, 13 * (8191.0 / 48.0), 100) };
    dl_live_ingest(&st, b, 3, 1, 60, lsink);
    CHECK(nlev == 2 && lev[1].st == 0x82 && lev[1].d1 == 73 && lev[1].slot == 1);
}

static void test_live_move_mono_cut(void)
{
    /* Move's own shape for a two-pad "chord" on one pad: On A, Off A, On B. */
    dl_live_t st; memset(&st, 0, sizeof st); st.primed = 1; nlev = 0;
    const double s = 8191.0 / 48.0;
    mm_live_rec_t r[] = { R(0, 38, 68, 4712), R(3, -2, 6 * s, 4712), R(1, 38, 0, 4712),
                          R(0, 38, 55, 4713), R(3, -2, 8 * s, 4713) };
    dl_live_ingest(&st, r, 5, 0, 60, lsink);
    CHECK(nlev == 3);
    CHECK(lev[0].st == 0x92 && lev[0].d1 == 66);
    CHECK(lev[1].st == 0x82 && lev[1].d1 == 66);
    CHECK(lev[2].st == 0x92 && lev[2].d1 == 68);
}

static void test_live_unrouted_still_advances(void)
{
    /* A note on a track that is not routed is SEEN: routing the track later
     * must not replay it from the persisting records. */
    dl_live_t st; memset(&st, 0, sizeof st); st.primed = 1; nlev = 0;
    mm_live_rec_t r[] = { R(0, 36, 90, 500) };
    dl_live_ingest(&st, r, 1, -1, 60, lsink);
    dl_live_ingest(&st, r, 1, 2, 60, lsink);
    CHECK(nlev == 0);
}

static void test_live_off_goes_to_start_slot(void)
{
    dl_live_t st; memset(&st, 0, sizeof st); st.primed = 1; nlev = 0;
    mm_live_rec_t on[] = { R(0, 36, 90, 600) };
    dl_live_ingest(&st, on, 1, 1, 60, lsink);           /* started on slot 1 */
    mm_live_rec_t off[] = { R(1, 36, 0, 600) };
    dl_live_ingest(&st, off, 1, 3, 60, lsink);          /* track switched: now slot 3 */
    CHECK(nlev == 2 && lev[1].slot == 1 && lev[1].st == 0x80);
}

static void test_live_id_reset(void)
{
    dl_live_t st; memset(&st, 0, sizeof st); st.primed = 1; nlev = 0;
    st.last_on = 900000;
    mm_live_rec_t r[] = { R(0, 36, 90, 5) };
    dl_live_ingest(&st, r, 1, 0, 60, lsink);
    CHECK(nlev == 1);
}

static void test_live_ignores_other_endpoints(void)
{
    dl_live_t st; memset(&st, 0, sizeof st); st.primed = 1; nlev = 0;
    mm_live_rec_t r[] = { R(0, 36, 90, 700) };
    r[0].ep = 1;                                        /* downstream copy, not live input */
    dl_live_ingest(&st, r, 1, 0, 60, lsink);
    CHECK(nlev == 0);
}

static void test_live_lost_off_cannot_stick(void)
{
    /* Move's NoteOff for the first note never reached us (overwritten before a
     * read): the next NoteOn on that pad must still end it, or it hangs. */
    dl_live_t st; memset(&st, 0, sizeof st); st.primed = 1; nlev = 0;
    mm_live_rec_t a[] = { R(0, 40, 90, 800) };
    dl_live_ingest(&st, a, 1, 0, 60, lsink);
    mm_live_rec_t b[] = { R(0, 40, 90, 801), R(3, -2, 5 * (8191.0 / 48.0), 801) };
    dl_live_ingest(&st, b, 2, 0, 60, lsink);
    CHECK(nlev == 3 && lev[1].st == 0x84 && lev[1].d1 == 60 && lev[2].st == 0x94 && lev[2].d1 == 65);
}

static void test_live_first_read_primes(void)
{
    /* Records already in the buffer when we first look are history, not notes. */
    dl_live_t st; memset(&st, 0, sizeof st); nlev = 0;
    mm_live_rec_t old[] = { R(0, 36, 90, 900), R(0, 37, 90, 901) };
    dl_live_ingest(&st, old, 2, 0, 60, lsink);
    CHECK(nlev == 0 && st.last_on == 901);
    mm_live_rec_t fresh[] = { R(0, 36, 90, 900), R(0, 37, 90, 901), R(0, 38, 90, 902) };
    dl_live_ingest(&st, fresh, 3, 0, 60, lsink);
    CHECK(nlev == 1 && lev[0].st == 0x92);
}

int main(void)
{
    test_live_first_read_primes();
    test_live_lost_off_cannot_stick();
    test_live_decode_captured();
    test_live_on_pitch_off();
    test_live_move_mono_cut();
    test_live_unrouted_still_advances();
    test_live_off_goes_to_start_slot();
    test_live_id_reset();
    test_live_ignores_other_endpoints();
    test_config_parse();
    test_wrapped_note_length();
    test_off_counts_the_offset_in_block();
    test_lane_channel_and_pitch();
    test_monophonic_lane();
    test_loop_wrap();
    test_stop_and_clip_change_release();
    test_unknown_notes_are_silence();
    test_min_one_block();
    printf("%s test_drum_lanes (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
