/*
 * shadow_slot_clip_phase() and shadow_lanes_step_phase() -- slot -> Move track
 * -> which clip, where in it we are, and where a held step lands in it.
 *
 * Both answer from Move's LIVE SONG MODEL (move_model.h) now. They used to
 * reconstruct it from session-pad LEDs, step-strip pixels, a playhead anchor
 * and a ~10 s stale Song.abl, and this file drove that ladder; what is pinned
 * here instead is the contract the chain relies on:
 *
 *   - identity (clip slot + fingerprint) is filled EVEN when the phase is
 *     unknown, because a p-lock is written with the transport stopped;
 *   - a stopped transport is phase UNKNOWN, never phase 0;
 *   - phase is CLIP time: the start marker plays in, then wraps inside the
 *     loop, from the exact launch beat;
 *   - a held step lands at the page's scroll + button * grid, triplets skip
 *     the dead fourth button, and a step past the clip's end is refused.
 *
 * shadow_chain_mgmt.c is INCLUDED, the way test_master_fx_permute.c does it:
 * 5000 lines with file-static state and no seam. mm_clip_position() is the
 * REAL one (move_model.c's pure half is linked), so the wrap arithmetic is
 * proved against its body rather than against a fake of it.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "shadow_chain_mgmt.c"

static int failures = 0;
static int checks = 0;
#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); \
                   printf("\n      (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* ------------------------------------------------------------------ fakes */
static move_model_t fake_model;
static double fake_now = -1.0;
int move_model_get(move_model_t *out) { *out = fake_model; return out->valid; }
double shadow_transport_beat_position(void) { return fake_now; }
int move_model_sync_active(void) { return fake_model.valid; }
void shadow_set_pages_ack_aligned(uint32_t gen) { (void)gen; }
uint32_t shadow_set_pages_published_gen(void) { return 0; }

/* The file-era tables are still referenced elsewhere in the unit; nothing
 * under test reads them any more. */
int shadow_clip_new_slot(int track) { (void)track; return -1; }
const clip_state_t *clip_state_current(void) { return NULL; }
const clip_regions_t *shadow_clip_regions(void) { return NULL; }
int shadow_transport_pulses = 0;
char sampler_current_set_name[128];
char sampler_current_set_uuid[64];
_Atomic int schwung_trace_on = 0;
uint32_t schwung_trace_intern(const char *name) { (void)name; return 0; }
uint64_t schwung_trace_now_ns(void) { return 0; }
void schwung_trace_span_explicit(uint32_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; }
int set_page_current = 0;
int set_page_read_persisted(void) { return 0; }
void shadow_batch_migrate_sets(void) {}
int shadow_load_config_from_dir(const char *dir) { (void)dir; return 0; }
void shadow_save_state(void) {}
void shadow_request_save_state(void) {}
int shadow_chain_midi_inject(const uint8_t *msg, int len) { (void)msg; (void)len; return 0; }
void unified_log(const char *source, int level, const char *fmt, ...)
{ (void)source; (void)level; (void)fmt; }

static void world(void)
{
    memset(&fake_model, 0, sizeof fake_model);
    fake_model.valid = 1;
    fake_model.clock_valid = 1;
    fake_model.playing = 1;
    fake_model.step_resolution = 1;
    fake_model.step_beats = 0.25;
    fake_now = 0.0;
}
static mm_clip_t *clip(int t, int s, double rs, double re, double ls, double le)
{
    mm_track_t *T = &fake_model.track[t];
    T->mode = 1;
    T->playing_slot = s;
    mm_clip_t *c = &T->slot[s];
    memset(c, 0, sizeof *c);
    c->exists = 1;
    c->clip_id = 1000 + t * 8 + s;
    c->region_start = rs; c->region_end = re;
    c->loop_start = ls; c->loop_end = le; c->loop_on = 1;
    c->scroll = 0.0;
    c->notes_len = 80; c->notes_hash = 0xfeed;
    return c;
}

#define POISON 424242.0
static int call(int slot, double *ph, double *len, int *cs, int *fpv, double *fp)
{
    *ph = POISON; *len = POISON; *cs = -99; *fpv = -99;
    for (int i = 0; i < 4; i++) fp[i] = POISON;
    return shadow_slot_clip_phase(slot, ph, len, cs, fpv, fp);
}


/* ------------------------------------------------------------ mix glide */
/* A slot's volume and pan GLIDE into the mix (shadow_mix_targets once per
 * block, shadow_mix_advance per frame). A scene morph sweeping the fader, an
 * override released, a mute -- each was a hard gain step every 128-frame block,
 * heard as zipper noise and clicks. Run a whole block the way the shim does
 * and bound the largest per-frame change. */
static float run_blocks(int slot, int blocks, float *max_step)
{
    for (int b = 0; b < blocks; b++) {
        shadow_mix_targets(slot);
        for (int f = 0; f < 128; f++) {
            const float before = shadow_chain_slots[slot].mix_vol;
            shadow_mix_advance(slot);
            const float d = fabsf(shadow_chain_slots[slot].mix_vol - before);
            if (d > *max_step) *max_step = d;
        }
    }
    return shadow_chain_slots[slot].mix_vol;
}

static void test_mix_glide(void)
{
    shadow_chain_slot_t *s = &shadow_chain_slots[0];
    memset(s, 0, sizeof *s);
    s->volume = 1.0f;
    shadow_solo_count = 0;
    float step = 0;
    CHECK(run_blocks(0, 1, &step) == 1.0f && step == 0.0f, "the first block SNAPS (nothing fades in from 0)");

    /* A scene takes the track from 1.0 to 0.2 in one move. */
    s->scene_volume = 0.2f; s->scene_volume_on = 1;
    step = 0;
    float v = run_blocks(0, 1, &step);
    CHECK(v > 0.5f, "after ONE block (2.9 ms) it is still on its way, not stepped: %f", v);
    v = run_blocks(0, 8, &step);
    CHECK(fabsf(v - 0.2f) < 0.01f, "within 1%% of the target after ~26 ms: %f", v);
    CHECK(step < 0.005f, "no per-frame jump bigger than 0.5%% (the step was 80%%): %f", step);

    /* Releasing the override glides back too. */
    s->scene_volume_on = 0; step = 0;
    v = run_blocks(0, 9, &step);
    CHECK(fabsf(v - 1.0f) < 0.01f && step < 0.005f, "release glides back: v=%f step=%f", v, step);

    /* Mute is a short fade now, not a click. */
    s->muted = 1; step = 0;
    v = run_blocks(0, 9, &step);
    CHECK(v < 0.01f && step < 0.005f, "mute fades out: v=%f step=%f", v, step);
    s->muted = 0;
    run_blocks(0, 20, &step);

    /* Pan: hard right by scene -- the LEFT gain glides down. */
    s->scene_pan = 1.0f; s->scene_pan_on = 1;
    shadow_mix_targets(0);
    CHECK(s->mix_pan_l > 0.99f, "pan has not stepped at the block start: %f", s->mix_pan_l);
    for (int b = 0; b < 12; b++) { shadow_mix_targets(0); for (int f = 0; f < 128; f++) shadow_mix_advance(0); }
    CHECK(s->mix_pan_l < 0.01f && s->mix_pan_r > 0.99f, "pan arrived: l=%f r=%f", s->mix_pan_l, s->mix_pan_r);
    memset(s, 0, sizeof *s);
}

int main(void)
{
    test_mix_glide();
    double ph, len, fp[4];
    int cs, fpv, rc;

    /* 1. Playing, launched at beat 8, a 4-beat loop: 1.5 beats in. */
    world();
    clip(2, 5, 0, 4, 0, 4);
    fake_model.track[2].start_beats = 8.0;
    fake_now = 9.5;
    rc = call(2, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 1 && fabs(ph - 1.5) < 1e-9 && fabs(len - 4.0) < 1e-9, "rc=%d ph=%f len=%f", rc, ph, len);
    CHECK(cs == 5 && fpv == 1 && fp[0] == 0.0 && fp[1] == 4.0 && fp[2] == 80.0 && fp[3] == (double)(0xfeed & 0x7fffffff),
          "identity cs=%d fpv=%d fp=%f,%f,%f,%f", cs, fpv, fp[0], fp[1], fp[2], fp[3]);

    /* 2. ...and it wraps inside the loop, from the LAUNCH beat, not from 0. */
    fake_now = 8.0 + 4.0 * 3 + 0.25;
    rc = call(2, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 1 && fabs(ph - 0.25) < 1e-9, "wrap ph=%f", ph);

    /* 3. A loop that begins later than the start marker: plays in, then loops
     *    [4, 12). Phase is CLIP time, so 13 beats in is 5, not 1. */
    world();
    clip(0, 0, 0, 12, 4, 12);
    fake_now = 13.0;
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 1 && fabs(ph - 5.0) < 1e-9 && fabs(len - 8.0) < 1e-9 && fp[0] == 4.0, "ph=%f len=%f fp0=%f", ph, len, fp[0]);

    /* 4. STOPPED: phase unknown (never 0), identity still filled -- a p-lock
     *    is written stopped and must know its clip. */
    world();
    clip(1, 3, 0, 8, 0, 8);
    fake_model.playing = 0;
    rc = call(1, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0 && isnan(ph) && cs == 3 && fpv == 1, "stopped rc=%d ph=%f cs=%d fpv=%d", rc, ph, cs, fpv);
    /* the shim's clock not running is the same answer */
    fake_model.playing = 1;
    fake_now = -1.0;
    rc = call(1, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0 && isnan(ph) && cs == 3, "no clock rc=%d", rc);
    /* ANOTHER FIRMWARE BUILD: the model's transport is unreadable
     * (clock_valid 0, playing 0), and the shim's clock alone decides -- a
     * Move update must not silence every lane. */
    fake_model.clock_valid = 0;
    fake_model.playing = 0;
    fake_now = 3.0;
    rc = call(1, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 1 && fabs(ph - 3.0) < 1e-9, "unknown build still plays: rc=%d ph=%f", rc, ph);
    fake_now = -1.0;
    rc = call(1, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0 && isnan(ph), "and still stops with the shim's clock");
    fake_model.clock_valid = 1;

    /* 5. No current clip: mode 2 (an empty slot was picked), mode 0, or the
     *    playing slot empty -> no identity at all. */
    world();
    clip(3, 2, 0, 4, 0, 4);
    fake_model.track[3].mode = 2;
    rc = call(3, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0 && cs == -1 && fpv == 0, "mode2 cs=%d fpv=%d", cs, fpv);
    fake_model.track[3].mode = 1;
    fake_model.track[3].slot[2].exists = 0;
    rc = call(3, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0 && cs == -1, "empty playing slot cs=%d", cs);

    /* 6. The model unavailable (another firmware): nothing, never a guess. */
    world();
    clip(0, 0, 0, 4, 0, 4);
    fake_model.valid = 0;
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0 && cs == -1 && fpv == 0 && isnan(ph), "invalid model rc=%d cs=%d", rc, cs);

    /* 7. Bounds and degenerate loops. */
    world();
    clip(0, 0, 0, 4, 4, 4);                       /* zero-length loop */
    rc = call(0, &ph, &len, &cs, &fpv, fp);
    CHECK(rc == 0 && cs == -1, "zero loop rc=%d cs=%d", rc, cs);
    CHECK(shadow_slot_clip_phase(-1, &ph, &len, &cs, &fpv, fp) == 0, "slot -1");
    CHECK(shadow_slot_clip_phase(CLIP_TRACKS, &ph, &len, &cs, &fpv, fp) == 0, "slot 4");

    /* ---- step -> clip time ------------------------------------------------ */
    double sp, clen, slen;
    world();
    mm_clip_t *c = clip(1, 0, 0, 16, 0, 16);
    c->scroll = 4.0;                              /* page 2 at 1/16 */
    rc = shadow_lanes_step_phase(1, 3, &sp, &clen, &slen);
    CHECK(rc == STEP_PLOCK_OK && fabs(sp - 4.75) < 1e-9 && clen == 16.0 && slen == 0.25,
          "step rc=%d sp=%f clen=%f slen=%f", rc, sp, clen, slen);

    /* the grid is the SONG's: at 1/32 the same button is half as far */
    fake_model.step_beats = 0.125;
    rc = shadow_lanes_step_phase(1, 3, &sp, NULL, NULL);
    CHECK(rc == STEP_PLOCK_OK && fabs(sp - 4.375) < 1e-9, "1/32 sp=%f", sp);

    /* triplets: 12 steps per page, every fourth button dead */
    fake_model.step_beats = 1.0 / 6.0;
    fake_model.step_triplet = 1;
    c->scroll = 0.0;
    rc = shadow_lanes_step_phase(1, 4, &sp, NULL, NULL);   /* button 4 = step 3 */
    CHECK(rc == STEP_PLOCK_OK && fabs(sp - 0.5) < 1e-9, "triplet sp=%f", sp);
    rc = shadow_lanes_step_phase(1, 3, &sp, NULL, NULL);   /* the dead one */
    CHECK(rc == STEP_PLOCK_BAD_INDEX && isnan(sp), "dead button rc=%d", rc);

    /* past the clip's end is refused (the "+" page beyond a clip) */
    fake_model.step_triplet = 0;
    fake_model.step_beats = 0.25;
    c->scroll = 16.0;
    rc = shadow_lanes_step_phase(1, 0, &sp, NULL, NULL);
    CHECK(rc == STEP_PLOCK_OUTSIDE_CLIP, "outside rc=%d", rc);

    /* works STOPPED -- which is how step editing is done */
    c->scroll = 0.0;
    fake_model.playing = 0;
    fake_now = -1.0;
    rc = shadow_lanes_step_phase(1, 2, &sp, NULL, NULL);
    CHECK(rc == STEP_PLOCK_OK && fabs(sp - 0.5) < 1e-9, "stopped step sp=%f", sp);

    /* no clip on the track yet: pending, not a phase */
    fake_model.track[1].mode = 2;
    rc = shadow_lanes_step_phase(1, 2, &sp, NULL, NULL);
    CHECK(rc == STEP_PLOCK_CLIP_PENDING, "pending rc=%d", rc);

    /* an audio clip has no step editor scroll */
    fake_model.track[1].mode = 1;
    c->scroll = -1.0;
    rc = shadow_lanes_step_phase(1, 2, &sp, NULL, NULL);
    CHECK(rc == STEP_PLOCK_NO_BAR, "audio rc=%d", rc);

    printf("test_slot_clip_phase: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
