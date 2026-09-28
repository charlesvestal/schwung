/* drum_lanes.c: the 16 Pitches notes Move never sends, added on the drum
 * track's own output channel -- the pad's own note, pitch as bend, CC 3
 * marking a 16 Pitches note -- plus every hit when the track's output is off.
 * Drives the sequenced and live paths and checks the MIDI they emit. */
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
/* Every test's output channel. ALL = the track's MIDI output is off, so every
 * hit is ours; AUG = it is on, so only the pitched ones are. */
#define CH 5
static const dl_out_t ALL = { CH, 1 }, AUG = { CH, 0 };
#define ON  (0x90 | CH)
#define OFF (0x80 | CH)
#define CC  (0xB0 | CH)
#define PBS (0xE0 | CH)

static int on_index(int note, int nth)
{
    int seen = 0;
    for (int i = 0; i < nev; i++)
        if (ev[i].st == ON && ev[i].d1 == note && seen++ == nth) return i;
    return -1;
}
/* The bend in force at `note`'s n-th NoteOn, in semitones (0 if none sent). */
static double bend_before_on(int note, int nth)
{
    int i = on_index(note, nth);
    if (i < 0) return NAN;
    for (int j = i - 1; j >= 0; j--)
        if (ev[j].st == PBS) return ((ev[j].d1 | (ev[j].d2 << 7)) - 8192) * 48.0 / 8191.0;
    return 0.0;
}
/* CC 3 in force at `note`'s n-th NoteOn (0 if none sent). */
static int cc3_before_on(int note, int nth)
{
    int i = on_index(note, nth);
    if (i < 0) return -1;
    for (int j = i - 1; j >= 0; j--)
        if (ev[j].st == CC && ev[j].d1 == DL_CC_PITCHED) return ev[j].d2;
    return 0;
}

/* ---- the MIDI shape ---------------------------------------------------- */

static void test_emit_shape(void)
{
    clear();
    dl_emit_on(sink, NULL, 4, 40, 90, 1, 13.0);
    CHECK(nev == 4);
    CHECK(ev[0].st == 0xB4 && ev[0].d1 == 3 && ev[0].d2 == 1);   /* CC 3 = pitched, first */
    CHECK(ev[1].st == 0xE4);                                       /* then the bend */
    CHECK(fabs((((ev[1].d1 | (ev[1].d2 << 7)) - 8192) * 48.0 / 8191.0) - 13.0) < 0.01);
    CHECK(ev[2].st == 0x94 && ev[2].d1 == 40 && ev[2].d2 == 90);   /* the PAD's note, never moved */
    CHECK(ev[3].st == 0xB4 && ev[3].d1 == 3 && ev[3].d2 == 0);     /* Move's next plain hit reads plain */
    clear();
    dl_emit_on(sink, NULL, 0, 36, 100, 0, 0.0);                    /* a plain hit: the note alone */
    CHECK(nev == 1 && ev[0].st == 0x90 && ev[0].d1 == 36);
    clear();
    dl_emit_on(sink, NULL, 2, 36, 100, 1, 60.0);                   /* past +48: clamped, not wrapped */
    CHECK(ev[1].d1 == 0x7F && ev[1].d2 == 0x7F);
    clear();
    dl_emit_off(sink, NULL, 15, 51, 0);
    CHECK(nev == 1 && ev[0].st == 0x8F && ev[0].d1 == 51);
    clear();
    dl_emit_off(sink, NULL, 15, 51, 1);                            /* a pitched off re-centres */
    CHECK(nev == 2 && ev[1].st == 0xEF && ev[1].d1 == 0 && ev[1].d2 == 64);
}

static void test_out_for(void)
{
    dl_out_t o = dl_out_for(2, 0);                                 /* Move sends on ch 1 */
    CHECK(o.ch == 0 && !o.plain);
    o = dl_out_for(2, -1);                                         /* output off: ours, ch 3 */
    CHECK(o.ch == 2 && o.plain);
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
static dl_out_t out;
static void run(dl_track_t *st, dl_window_t w, double pos, double blk, int nblocks)
{
    for (int b = 0; b < nblocks; b++) {
        cur_block = b;
        double p = pos + b * blk;
        if (w.loop && p >= w.le) p = w.ls + fmod(p - w.ls, w.le - w.ls);
        dl_block(st, &clip, w, p, blk, out, sink, NULL);
    }
}
#define BLK (1.0 / 64.0)
#define W4 ((dl_window_t){ 0.0, 4.0, 1 })
#define W8 ((dl_window_t){ 0.0, 8.0, 1 })

static void test_lane_notes_and_pitch(void)
{
    reset_clip(7);
    add(36, 0.0, 0.25, 0.0f, 100); clip.note[0].has_pitch = 0;    /* plain */
    add(40, 0.5, 0.25, 13.0f, 90);                                 /* 16 Pitches +13 */
    add(51, 1.0, 0.25, -2.0f, 127);                                /* last lane, -2 */
    add(52, 1.5, 0.25, 0.0f, 100);                                 /* not a drum cell: ignored */
    dl_track_t st; memset(&st, 0, sizeof st); clear(); out = ALL;
    run(&st, W4, 0.0, BLK, 4 * 64);
    CHECK(count(ON, 36) == 1 && cc3_before_on(36, 0) == 0 && fabs(bend_before_on(36, 0)) < 0.01);
    CHECK(count(ON, 40) == 1 && cc3_before_on(40, 0) == 1 && fabs(bend_before_on(40, 0) - 13.0) < 0.01);
    CHECK(count(ON, 51) == 1 && fabs(bend_before_on(51, 0) + 2.0) < 0.01);
    CHECK(count(ON, -1) == 3);                                     /* one channel for every pad */
    CHECK(first_block(ON, 40) == 32);
    CHECK(count(OFF, 36) == 1 && count(OFF, 40) == 1 && count(OFF, 51) == 1);
    CHECK(first_block(OFF, 36) == 16);
}

static void test_augment_sends_only_pitched(void)
{
    reset_clip(17);
    add(36, 0.0, 0.25, 0.0f, 100); clip.note[0].has_pitch = 0;    /* Move sends this one */
    add(40, 0.5, 0.25, 7.0f, 90);                                  /* and never this one */
    dl_track_t st; memset(&st, 0, sizeof st); clear(); out = AUG;
    run(&st, W4, 0.0, BLK, 4 * 64);
    CHECK(count(ON, 36) == 0 && count(OFF, 36) == 0);
    CHECK(count(ON, 40) == 1 && cc3_before_on(40, 0) == 1 && fabs(bend_before_on(40, 0) - 7.0) < 0.01);
    CHECK(count(OFF, 40) == 1);
    /* CC 3 is back to 0 straight after the NoteOn, and the bend after the off. */
    const int i = on_index(40, 0);
    CHECK(i >= 0 && ev[i + 1].st == CC && ev[i + 1].d2 == 0);
    CHECK(ev[nev - 1].st == PBS && ev[nev - 1].d1 == 0 && ev[nev - 1].d2 == 64);
}

static void test_augment_plain_supersedes_pitched(void)
{
    /* A plain hit on a lane still sounding a pitched note: Move sends that
     * NoteOn itself, so we only undo the bend -- an off would kill Move's note. */
    reset_clip(18);
    add(38, 0.0, 1.0, 5.0f, 100);
    add(38, 0.5, 0.25, 0.0f, 100); clip.note[1].has_pitch = 0;
    dl_track_t st; memset(&st, 0, sizeof st); clear(); out = AUG;
    run(&st, W4, 0.0, BLK, 2 * 64);
    CHECK(count(ON, 38) == 1 && count(OFF, 38) == 0);
    CHECK(ev[nev - 1].st == PBS && ev[nev - 1].d2 == 64);
}

static void test_monophonic_lane(void)
{
    reset_clip(8);
    add(38, 0.0, 1.0, 0.0f, 100);
    add(38, 0.5, 0.25, 5.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear(); out = ALL;
    run(&st, W4, 0.0, BLK, 2 * 64);
    CHECK(count(ON, 38) == 2 && count(OFF, 38) == 2);
    CHECK(first_block(OFF, 38) == 32);                             /* the first is cut at 0.5 */
    CHECK(fabs(bend_before_on(38, 1) - 5.0) < 0.01);
    int off_i = -1;
    for (int i = 0; i < nev; i++) if (ev[i].st == OFF && off_i < 0) off_i = i;
    CHECK(off_i >= 0 && on_index(38, 1) > off_i);                  /* the cut comes first */
}

static void test_other_pads_sound_together(void)
{
    reset_clip(19);
    add(36, 0.0, 1.0, 3.0f, 100);
    add(37, 0.0, 1.0, -5.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear(); out = AUG;
    run(&st, W4, 0.0, BLK, 8);
    CHECK(count(ON, 36) == 1 && count(ON, 37) == 1 && count(OFF, -1) == 0);
    CHECK(fabs(bend_before_on(36, 0) - 3.0) < 0.01 && fabs(bend_before_on(37, 0) + 5.0) < 0.01);
}

static void test_loop_wrap(void)
{
    reset_clip(9);
    add(36, 0.0, 0.1, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear();
    for (int b = 0; b < 10; b++) {
        cur_block = b;
        dl_block(&st, &clip, (dl_window_t){ 0.0, 1.0, 1 }, fmod(b * 0.3, 1.0), 0.3, ALL, sink, NULL);
    }
    CHECK(count(ON, 36) == 3);
}

static void test_wrapped_note_length(void)
{
    reset_clip(15);
    add(36, 0.0, 0.25, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear();
    const double starts[3] = { 0.9, 0.2, 0.5 };
    for (int b = 0; b < 3; b++) {
        cur_block = b;
        dl_block(&st, &clip, (dl_window_t){ 0.0, 1.0, 1 }, starts[b], 0.3, ALL, sink, NULL);
    }
    CHECK(first_block(ON, 36) == 0 && first_block(OFF, 36) == 2);
}

static void test_off_counts_the_offset_in_block(void)
{
    reset_clip(14);
    add(36, 0.05, 0.1, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear();
    for (int b = 0; b < 4; b++) {
        cur_block = b;
        dl_block(&st, &clip, W4, b * 0.1, 0.1, ALL, sink, NULL);
    }
    CHECK(first_block(ON, 36) == 0 && first_block(OFF, 36) == 2);
}

static void test_stop_and_clip_change_release(void)
{
    reset_clip(10);
    add(36, 0.0, 4.0, 0.0f, 100);
    add(37, 0.0, 4.0, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear(); out = ALL;
    run(&st, W8, 0.0, BLK, 4);
    CHECK(count(ON, 36) == 1 && count(ON, 37) == 1 && count(OFF, -1) == 0);
    cur_block = 99;
    dl_block(&st, &clip, W8, -1.0, BLK, ALL, sink, NULL);          /* stopped */
    CHECK(count(OFF, 36) == 1 && count(OFF, 37) == 1);
    clear();
    run(&st, W8, 0.0, BLK, 2);
    reset_clip(11);                                                /* another clip launched */
    add(40, 2.0, 1.0, 0.0f, 100);
    cur_block = 50;
    dl_block(&st, &clip, W8, 0.5, BLK, ALL, sink, NULL);
    CHECK(count(OFF, 36) == 1 && count(OFF, 37) == 1);
}

static void test_off_follows_its_channel(void)
{
    /* The track's output channel changes under a sounding note: the off goes
     * where the on went. */
    reset_clip(16);
    add(36, 0.0, 4.0, 1.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear(); out = ALL;
    run(&st, W8, 0.0, BLK, 2);
    const dl_out_t moved = { 9, 1 };
    dl_block(&st, &clip, W8, -1.0, BLK, moved, sink, NULL);
    CHECK(count(OFF, 36) == 1 && count(0x89, -1) == 0);
}

static void test_unknown_notes_are_silence(void)
{
    reset_clip(12);
    add(36, 0.0, 4.0, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear(); out = ALL;
    run(&st, W8, 0.0, BLK, 2);
    clip.valid = 0;
    cur_block = 5;
    dl_block(&st, &clip, W8, 0.1, BLK, ALL, sink, NULL);
    CHECK(count(OFF, 36) == 1);
    clear();
    run(&st, W8, 0.0, BLK, 2);
    CHECK(nev == 0);
}

static void test_min_one_block(void)
{
    reset_clip(13);
    add(36, 0.0, 0.001, 0.0f, 100);
    dl_track_t st; memset(&st, 0, sizeof st); clear(); out = ALL;
    run(&st, W4, 0.0, BLK, 3);
    CHECK(first_block(ON, 36) == 0 && first_block(OFF, 36) == 1);
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
    dl_live_ingest(&st, a, 2, 1, AUG, sink, NULL);
    CHECK(count(ON, 38) == 1 && cc3_before_on(38, 0) == 1 && fabs(bend_before_on(38, 0) - 13.0) < 0.01);
    dl_live_ingest(&st, a, 2, 1, AUG, sink, NULL);                 /* the same records again */
    CHECK(count(ON, 38) == 1);
    mm_live_rec_t b[] = { R(1, 38, 0, 100), R(0, 38, 74, 100), R(3, -2, PB(13), 100) };
    dl_live_ingest(&st, b, 3, 1, AUG, sink, NULL);
    CHECK(count(OFF, 38) == 1 && ev[nev - 1].st == PBS && ev[nev - 1].d2 == 64);
}

static void test_live_plain_vs_pitched_zero(void)
{
    dl_live_t st = primed(); clear();
    mm_live_rec_t p[] = { R(0, 36, 90, 200) };                     /* no PNCC: plain */
    dl_live_ingest(&st, p, 1, 1, ALL, sink, NULL);
    mm_live_rec_t z[] = { R(0, 37, 90, 201), R(3, -2, 0.0f, 201) }; /* 16 Pitches at +0 */
    dl_live_ingest(&st, z, 2, 1, ALL, sink, NULL);
    CHECK(cc3_before_on(36, 0) == 0 && cc3_before_on(37, 0) == 1); /* told apart by CC 3 */
    CHECK(fabs(bend_before_on(36, 0)) < 0.01 && fabs(bend_before_on(37, 0)) < 0.01);
}

static void test_live_augment_skips_plain(void)
{
    dl_live_t st = primed(); clear();
    mm_live_rec_t p[] = { R(0, 36, 90, 300) };                     /* Move sends this itself */
    dl_live_ingest(&st, p, 1, 1, AUG, sink, NULL);
    mm_live_rec_t q[] = { R(1, 36, 0, 300) };
    dl_live_ingest(&st, q, 1, 1, AUG, sink, NULL);
    CHECK(nev == 0);
}

static void test_live_move_mono_cut(void)
{
    dl_live_t st = primed(); clear();
    mm_live_rec_t r[] = { R(0, 38, 68, 4712), R(3, -2, PB(6), 4712), R(1, 38, 0, 4712),
                          R(0, 38, 55, 4713), R(3, -2, PB(8), 4713) };
    dl_live_ingest(&st, r, 5, 1, AUG, sink, NULL);
    CHECK(count(ON, 38) == 2 && count(OFF, 38) == 1);
    CHECK(fabs(bend_before_on(38, 0) - 6.0) < 0.01 && fabs(bend_before_on(38, 1) - 8.0) < 0.01);
}

static void test_live_lost_off_cannot_stick(void)
{
    dl_live_t st = primed(); clear();
    mm_live_rec_t a[] = { R(0, 40, 90, 800) };
    dl_live_ingest(&st, a, 1, 1, ALL, sink, NULL);
    mm_live_rec_t b[] = { R(0, 40, 90, 801), R(3, -2, PB(5), 801) };
    dl_live_ingest(&st, b, 2, 1, ALL, sink, NULL);
    CHECK(count(OFF, 40) == 1 && count(ON, 40) == 2);              /* the lost off is supplied */
    int off_i = -1;
    for (int i = 0; i < nev; i++) if (ev[i].st == OFF && off_i < 0) off_i = i;
    CHECK(off_i >= 0 && on_index(40, 1) > off_i);
}

static void test_live_unrouted_still_advances(void)
{
    dl_live_t st = primed(); clear();
    mm_live_rec_t r[] = { R(0, 36, 90, 500) };
    dl_live_ingest(&st, r, 1, 0, ALL, sink, NULL);
    dl_live_ingest(&st, r, 1, 1, ALL, sink, NULL);
    CHECK(nev == 0);
}

static void test_live_off_after_track_switch(void)
{
    /* Started while routed; the Off arrives after the selection moved away. */
    dl_live_t st = primed(); clear();
    mm_live_rec_t on[] = { R(0, 36, 90, 600) };
    dl_live_ingest(&st, on, 1, 1, ALL, sink, NULL);
    mm_live_rec_t off[] = { R(1, 36, 0, 600) };
    dl_live_ingest(&st, off, 1, 0, ALL, sink, NULL);
    CHECK(count(OFF, 36) == 1);
}

static void test_live_id_reset(void)
{
    dl_live_t st = primed(); clear();
    st.last_on = 900000;
    mm_live_rec_t r[] = { R(0, 36, 90, 5) };
    dl_live_ingest(&st, r, 1, 1, ALL, sink, NULL);
    CHECK(count(ON, 36) == 1);
}

static void test_live_ignores_other_endpoints(void)
{
    dl_live_t st = primed(); clear();
    mm_live_rec_t r[] = { R(0, 36, 90, 700) };
    r[0].ep = 1;
    dl_live_ingest(&st, r, 1, 1, ALL, sink, NULL);
    CHECK(nev == 0);
}

static void test_live_first_read_primes(void)
{
    dl_live_t st; memset(&st, 0, sizeof st); clear();
    mm_live_rec_t old[] = { R(0, 36, 90, 900), R(0, 37, 90, 901) };
    dl_live_ingest(&st, old, 2, 1, ALL, sink, NULL);
    CHECK(nev == 0 && st.last_on == 901);
    mm_live_rec_t fresh[] = { R(0, 36, 90, 900), R(0, 37, 90, 901), R(0, 38, 90, 902) };
    dl_live_ingest(&st, fresh, 3, 1, ALL, sink, NULL);
    CHECK(count(ON, 38) == 1 && count(ON, -1) == 1);
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

static void test_tracks_wanted(void)
{
    /* Set 5's routing: tracks 1 and 2 send on ch 1 and 2, 3 and 4 have no
     * output (so theirs is track+1: ch 3 and 4). */
    const int ch[MM_TRACKS] = { 0, 1, 2, 3 };
    int on[MM_TRACKS];
    const int rx[4] = { 0, 1, 2, 3 };
    const int none[4] = { 0, 0, 0, 0 }, slot2[4] = { 0, 1, 0, 0 };
    dl_tracks_wanted(4, rx, none, ch, on);
    CHECK(!on[0] && !on[1] && !on[2] && !on[3]);
    dl_tracks_wanted(4, rx, slot2, ch, on);                        /* slot 2 hears track 2 */
    CHECK(!on[0] && on[1] && !on[2] && !on[3]);
    const int rx_all[4] = { 0, -1, 2, 3 };
    dl_tracks_wanted(4, rx_all, slot2, ch, on);                    /* ... or All: every track */
    CHECK(on[0] && on[1] && on[2] && on[3]);
    const int moved[MM_TRACKS] = { 1, 5, 2, 3 };                   /* track 1 now sends on ch 2 */
    dl_tracks_wanted(4, rx, slot2, moved, on);
    CHECK(on[0] && !on[1]);
}

int main(void)
{
    test_emit_shape();
    test_out_for();
    test_lane_notes_and_pitch();
    test_augment_sends_only_pitched();
    test_augment_plain_supersedes_pitched();
    test_monophonic_lane();
    test_other_pads_sound_together();
    test_off_follows_its_channel();
    test_loop_wrap();
    test_wrapped_note_length();
    test_off_counts_the_offset_in_block();
    test_stop_and_clip_change_release();
    test_unknown_notes_are_silence();
    test_min_one_block();
    test_live_decode_captured();
    test_live_on_pitch_off();
    test_live_plain_vs_pitched_zero();
    test_live_augment_skips_plain();
    test_live_move_mono_cut();
    test_live_lost_off_cannot_stick();
    test_live_unrouted_still_advances();
    test_live_off_after_track_switch();
    test_live_id_reset();
    test_live_ignores_other_endpoints();
    test_live_first_read_primes();
    test_decoder_has_pitch();
    test_tracks_wanted();
    printf("%s test_drum_lanes (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
