/*
 * The step menu, glued to the device: hold one step, press Menu (step_menu.h
 * is the gesture; docs/plans/2026-09-30-step-menu-design.md the design).
 *
 * Three threads, each doing only what it can:
 *
 *  - MODEL THREAD: the edited clip's notes are only decodable there
 *    (move_model_edited_notes), so it publishes the notes around the
 *    displayed page through a seqlock -- step_menu_publish_page().
 *  - SPI CALLBACK: the gesture (step_menu_on_input, from the post-transfer
 *    control scan) and the Chance edit, which writes `chance:notes` straight
 *    into the slot's chain -- whose set_param IS the callback anyway.
 *  - shadow_ui: draws a card from shadow_control_t.step_menu_* and writes
 *    nothing back.
 *
 * Length and Velocity are MOVE's: the jog passes through (Length) or is
 * rewritten into a Volume detent (Velocity) and Move edits its own note. What
 * the card shows for them is read back from the model, so it is Move's truth.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "step_menu.h"
#include "step_menu_glue.h"
#include "step_chance.h"
#include "move_model.h"
#include "shadow_constants.h"
#include "shadow_chain_mgmt.h"
#include "shadow_led_queue.h"

/* Enough for a dense page: 16 steps x a four-note chord, plus nudged
 * neighbours. A page denser than this loses its LAST notes from the menu, and
 * only from the menu -- the chain gates by what it stores, not by this. */
#define SM_PAGE_MAX 96

typedef struct {
    int      valid;
    int      track, row;
    double   scroll, step_beats, clip_len;
    int      triplet;
    int      n;
    int      truncated;       /* more notes in the window than SM_PAGE_MAX */
    double   lo, hi;          /* the window the notes were taken from */
    sm_note_t notes[SM_PAGE_MAX];
} sm_page_t;

static sm_page_t g_page_buf;
static atomic_uint g_page_seq;    /* odd while the writer is inside */

/* MODEL THREAD. The notes of the clip Move's step editor shows, around the
 * page it shows. Called every model tick; a clip with no notes (or none
 * decodable) publishes valid = 0 rather than an empty page, so the menu can
 * tell "no note on this step" from "we cannot see the clip". */
void step_menu_publish_page(const move_model_t *m, const mm_note_t *notes, int n,
                            const mm_clip_ref_t *ref)
{
    static sm_page_t w;
    memset(&w, 0, sizeof w);
    if (m && m->valid && ref && ref->valid && n >= 0 && notes &&
        ref->track >= 0 && ref->track < MM_TRACKS && ref->slot >= 0 && ref->slot < MM_SLOTS &&
        m->step_beats > 0.0) {
        const mm_clip_t *c = &m->track[ref->track].slot[ref->slot];
        if (c->exists && c->scroll >= 0.0) {
            w.valid = 1;
            w.track = ref->track;
            w.row = ref->slot;
            w.scroll = c->scroll;
            w.step_beats = m->step_beats;
            w.triplet = m->step_triplet;
            w.clip_len = c->loop_on ? c->loop_end : c->region_end;
            const double lo = w.scroll - w.step_beats;
            const double hi = w.scroll + 17.0 * w.step_beats;
            w.lo = lo; w.hi = hi;
            for (int i = 0; i < n; i++) {
                if (notes[i].start < lo || notes[i].start >= hi) continue;
                if (w.n >= SM_PAGE_MAX) { w.truncated = 1; break; }
                sm_note_t *o = &w.notes[w.n++];
                o->id = notes[i].id;
                o->start = notes[i].start;
                o->dur = notes[i].dur;
                o->vel = notes[i].vel;
                o->pitch = (uint8_t)(notes[i].pitch & 0x7F);
                /* Move's length cap, from the WHOLE clip: the next note of the
                 * same pitch may be pages away. */
                double lim = w.clip_len;
                for (int q = 0; q < n; q++) {
                    if (q == i || notes[q].pitch != notes[i].pitch) continue;
                    if (notes[q].start > notes[i].start + 1e-9 && notes[q].start < lim)
                        lim = notes[q].start;
                }
                o->cap = lim - notes[i].start;
            }
        }
    }
    /* Only a CHANGE is published: the seq is what the callback follows Move's
     * edits on, and a seq bumped every tick would re-run that every 8th frame
     * forever. Compared as bytes -- `w` is memset, so padding is zero too. */
    static sm_page_t last;
    static int have_last;
    if (have_last && memcmp(&last, &w, sizeof w) == 0) return;
    memcpy(&last, &w, sizeof w);
    have_last = 1;
    unsigned s = atomic_load_explicit(&g_page_seq, memory_order_relaxed);
    atomic_store_explicit(&g_page_seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    memcpy(&g_page_buf, &w, sizeof w);
    atomic_store_explicit(&g_page_seq, s + 2, memory_order_release);
}

/* SPI CALLBACK. A consistent copy, or 0 after three torn tries -- the menu
 * then simply keeps what it last drew. */
static int page_read(sm_page_t *out)
{
    for (int tries = 0; tries < 3; tries++) {
        unsigned a = atomic_load_explicit(&g_page_seq, memory_order_acquire);
        if (a & 1u) continue;
        memcpy(out, &g_page_buf, sizeof *out);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&g_page_seq, memory_order_relaxed) == a) return 1;
    }
    return 0;
}

static uint64_t sm_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ---- callback state ---------------------------------------------------- */



static sm_state_t g_sm;
static int g_dirty;               /* the card needs republishing */
static unsigned g_last_page_seq;
static int g_frames;
static sm_page_t g_pg;            /* the callback's copy; static, off the stack */

/* Move's selected drum CELL: the left-4x4 pad it lights 122. -1 if none. */
static int selected_voice_pitch(void)
{
    for (int pad = 68; pad <= 99; pad++) {
        const int p = sm_drum_cell_pitch(pad);
        if (p >= 0 && led_queue_get_note_led_color(pad) == 122) return p;
    }
    return -1;
}

static void *slot_instance(int track)
{
    if (track < 0 || track >= SHADOW_CHAIN_INSTANCES) return NULL;
    if (!shadow_plugin_v2 || !shadow_chain_slots[track].active) return NULL;
    return shadow_chain_slots[track].instance;
}

/* Does the track's slot hold a Schwung SYNTH? A chain with no sound
 * generator is a Move-only track as far as chance is concerned. */
static int slot_has_synth(int track)
{
    void *inst = slot_instance(track);
    if (!inst || !shadow_plugin_v2->get_param) return 0;
    char buf[64];
    const int n = shadow_plugin_v2->get_param(inst, "synth_module", buf, sizeof buf);
    return n > 0 && buf[0] != 0;
}

/* The notes the menu is about on `button`: nearest-step, then scoped to the
 * selected drum voice. */
static int button_notes(const sm_page_t *pg, int button, int *idx, int max, int voice)
{
    int k = sm_button_notes(pg->notes, pg->n, pg->scroll, pg->step_beats, pg->triplet,
                            pg->clip_len, button, idx, max);
    return sm_scope_voice(pg->notes, idx, k, voice);
}

/* A note's condition, asked of the chain. SC_ALWAYS on any failure. */
static int note_cond(void *inst, int row, int64_t id)
{
    if (!inst || !shadow_plugin_v2->get_param) return SC_ALWAYS;
    char key[64], buf[16];
    snprintf(key, sizeof key, "chance:of:%d:%lld", row, (long long)id);
    int n = shadow_plugin_v2->get_param(inst, key, buf, sizeof buf);
    if (n <= 0) return SC_ALWAYS;
    buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = 0;
    int v = atoi(buf);
    return sc_valid(v) ? v : SC_ALWAYS;
}

static void apply_chance(int dir)
{
    if (!dir || !page_read(&g_pg) || !g_pg.valid) return;
    void *inst = slot_instance(g_pg.track);
    if (!inst || !shadow_plugin_v2->set_param || !slot_has_synth(g_pg.track)) return;
    int idx[16];
    const int k = button_notes(&g_pg, g_sm.step, idx, 16, selected_voice_pitch());
    if (k <= 0) return;
    const int cur = note_cond(inst, g_pg.row, g_pg.notes[idx[0]].id);
    const int nxt = sm_step_cond(cur, dir, sc_count());
    if (nxt == cur) return;
    char val[16 * 48 + 64];
    /* The step's own position is the trig's group: its notes roll together. */
    double ph = 0.0;
    step_plock_phase_from_scroll(g_pg.scroll, g_sm.step, g_pg.step_beats, g_pg.triplet,
                                 g_pg.clip_len, &ph);
    int w = snprintf(val, sizeof val, "%d %d g=%.17g", g_pg.row, nxt, ph);
    for (int i = 0; i < k && w < (int)sizeof val - 48; i++) {
        const sm_note_t *nt = &g_pg.notes[idx[i]];
        w += snprintf(val + w, sizeof val - (size_t)w, " %lld %d %.17g",
                      (long long)nt->id, nt->pitch, nt->start);
    }
    shadow_plugin_v2->set_param(inst, "chance:notes", val);
    g_dirty = 1;
}

/* SPI CALLBACK: one cable-0 MIDI_IN event. SM_SWALLOW / SM_REWRITE / SM_PASS. */
int step_menu_on_input(uint8_t status, uint8_t d1, uint8_t d2, uint8_t out[3],
                       uint32_t held_mask, int shift_held, int eligible, uint64_t now_ms)
{
    const uint8_t was_open = g_sm.open, was_field = g_sm.field, was_step = g_sm.step;
    int dir = 0;
    const int a = sm_on_input(&g_sm, held_mask, shift_held, eligible, status, d1, d2, out, &dir,
                              now_ms);
    if (dir) apply_chance(dir);
    if (g_sm.open != was_open || g_sm.field != was_field || g_sm.step != was_step) g_dirty = 1;
    /* Length and Velocity are edited by MOVE; the model reports the result a
     * tick or two later, and the page seq change republishes it then. */
    return a;
}

/* FOLLOW MOVE'S EDITS on the page it shows. A condition is keyed by note id
 * but matched at playback by pitch + position, so a note Move NUDGED or
 * re-pitched must carry its new position into the store, and a note Move
 * DELETED must leave it -- or the next note placed on that step at that pitch
 * inherits a condition nobody gave it. Only the window we can see, and never
 * from a page that was truncated or could not be decoded: absence from a list
 * we did not finish reading says nothing. */
static void follow_page_edits(const sm_page_t *pg)
{
    if (!pg->valid || pg->truncated) return;
    void *inst = slot_instance(pg->track);
    if (!inst || !shadow_plugin_v2->set_param) return;
    char val[64 + SM_PAGE_MAX * 21];
    int w = snprintf(val, sizeof val, "%d %.17g %.17g", pg->row, pg->lo, pg->hi);
    for (int i = 0; i < pg->n; i++) {
        const sm_note_t *nt = &pg->notes[i];
        w += snprintf(val + w, sizeof val - (size_t)w, " %lld", (long long)nt->id);
        char mv[96];
        snprintf(mv, sizeof mv, "%d %lld %d %.17g", pg->row, (long long)nt->id,
                 nt->pitch, nt->start);
        if (note_cond(inst, pg->row, nt->id) != SC_ALWAYS) {
            shadow_plugin_v2->set_param(inst, "chance:move", mv);
        } else {
            /* A condition COPIED here (paste, Double Loop, clip copy) waits
             * under a synthetic id: it becomes this note's. BEFORE the prune,
             * which would otherwise take a copy nobody had claimed yet. */
            shadow_plugin_v2->set_param(inst, "chance:adopt", mv);
        }
    }
    shadow_plugin_v2->set_param(inst, "chance:prune", val);
}

/* How many detents Move's value can still move, up and down, for every note
 * the held step edits -- the wind-up guard's seed (step_menu.h). Length moves
 * 0.1 step a detent between 0.1 step and the note's cap; Velocity 1 a detent
 * between 1 and 127. A chord moves until its LAST note stops, as Move clamps
 * each on its own. */
static void seed_bounds(const sm_page_t *pg, const int *idx, int k)
{
    if (k <= 0 || !(pg->step_beats > 0.0)) { g_sm.bound_known = 0; return; }
    int32_t up = 0, down = 0;
    for (int q = 0; q < k; q++) {
        const sm_note_t *nt = &pg->notes[idx[q]];
        int32_t u, d;
        if (g_sm.field == SM_FIELD_LENGTH) {
            const double unit = 0.1 * pg->step_beats;
            /* ROUNDED, never floored: Move builds a length by adding 0.025 q
             * again and again, so "15.5 steps" is 3.8750001 and a floor of
             * 4.99999 stopped the jog one detent short of the cap (15.9, on
             * hardware). Move moves in exact units and clamps at the cap, so
             * the nearest whole count is the true one. */
            u = (int32_t)lround((nt->cap - nt->dur) / unit);
            d = (int32_t)lround((nt->dur - unit) / unit);
        } else {
            u = 127 - (int32_t)(nt->vel + 0.5f);
            d = (int32_t)(nt->vel + 0.5f) - 1;
        }
        if (u > up) up = u;
        if (d > down) down = d;
    }
    sm_bound_seed(&g_sm, up, down);
}

/* SPI CALLBACK, once per frame after the scan: close a menu whose step went
 * away, follow Move's edits, and republish the card when anything it draws
 * changed. */
void step_menu_frame(shadow_control_t *ctl, uint32_t held_mask, int eligible)
{
    if (!ctl) return;
    /* Move's edits, whether or not the menu is open -- a nudge with the step
     * held and the menu closed is the ordinary case. At most every 8th frame. */
    {
        static unsigned followed_seq;
        static int follow_frames;
        const unsigned ps = atomic_load_explicit(&g_page_seq, memory_order_relaxed);
        if (ps != followed_seq && (++follow_frames & 7) == 0 && page_read(&g_pg)) {
            followed_seq = ps;
            follow_page_edits(&g_pg);
        }
    }
    const uint8_t was_open = g_sm.open;
    sm_validate(&g_sm, held_mask, eligible);
    if (g_sm.open != was_open) g_dirty = 1;

    if (!g_sm.open) {
        if (ctl->step_menu_open) { ctl->step_menu_open = 0; ctl->step_menu_seq++; }
        return;
    }
    /* Move's own edits arrive through the model: follow its page seq, but no
     * more often than every 8th frame (~23 ms) -- each republish asks the
     * chain for up to 16 x chord conditions. */
    const unsigned ps = atomic_load_explicit(&g_page_seq, memory_order_relaxed);
    if (ps != g_last_page_seq && (++g_frames & 7) == 0) g_dirty = 1;
    if (!g_dirty) return;
    if (!page_read(&g_pg)) return;           /* torn: try next frame */
    g_last_page_seq = ps;
    g_dirty = 0;

    ctl->step_menu_field = g_sm.field;
    ctl->step_menu_step = g_sm.step;
    ctl->step_menu_track = (uint8_t)(g_pg.valid ? g_pg.track : 0);
    ctl->step_menu_flags = (g_pg.valid && !slot_has_synth(g_pg.track)) ? SM_FLAG_NO_SYNTH : 0;
    ctl->step_menu_cond = SM_CELL_EMPTY;
    ctl->step_menu_vel = 0;
    ctl->step_menu_len_c = 0;
    void *inst = g_pg.valid ? slot_instance(g_pg.track) : NULL;
    const int voice = selected_voice_pitch();
    int idx[16];
    for (int b = 0; b < 16; b++) {
        double ph;
        if (!g_pg.valid || step_plock_phase_from_scroll(g_pg.scroll, b, g_pg.step_beats,
                                                        g_pg.triplet, g_pg.clip_len,
                                                        &ph) != STEP_PLOCK_OK) {
            ctl->step_menu_page[b] = SM_CELL_OFF;
            continue;
        }
        const int k = button_notes(&g_pg, b, idx, 16, voice);
        if (k <= 0) { ctl->step_menu_page[b] = SM_CELL_EMPTY; continue; }
        const int c = note_cond(inst, g_pg.row, g_pg.notes[idx[0]].id);
        ctl->step_menu_page[b] = (uint8_t)c;
        if (b == g_sm.step && g_sm.field != SM_FIELD_CHANCE &&
            (!g_sm.bound_known || sm_now_ms() - g_sm.jog_ms > 150)) {
            /* Re-seed only while the jog RESTS: mid-spin the model lags the
             * detents already sent, and the local count is the truth. */
            seed_bounds(&g_pg, idx, k);
        }
        if (b == g_sm.step) {
            /* Every note Move's own hold-step edit touches: a chord is a
             * RANGE ("2.0-16.0"), because Move clamps each note on its own. */
            double dmin = 1e9, dmax = 0; float vmin = 1e9f, vmax = 0;
            for (int q = 0; q < k; q++) {
                const sm_note_t *nt = &g_pg.notes[idx[q]];
                if (nt->dur < dmin) dmin = nt->dur;
                if (nt->dur > dmax) dmax = nt->dur;
                if (nt->vel < vmin) vmin = nt->vel;
                if (nt->vel > vmax) vmax = nt->vel;
            }
            #define SM_C(x) ((uint16_t)((x) < 0 ? 0 : (x) > 65535 ? 65535 : (x)))
            #define SM_V(x) ((uint8_t)((x) < 0 ? 0 : (x) > 127 ? 127 : (x)))
            ctl->step_menu_cond = (uint8_t)c;
            ctl->step_menu_vel = SM_V(vmin);
            ctl->step_menu_vel_max = SM_V(vmax);
            ctl->step_menu_len_c = SM_C(dmin / g_pg.step_beats * 100.0 + 0.5);
            ctl->step_menu_len_max_c = SM_C(dmax / g_pg.step_beats * 100.0 + 0.5);
            #undef SM_C
            #undef SM_V
        }
    }
    ctl->step_menu_open = 1;
    ctl->step_menu_seq++;
}

int step_menu_open_step(void)
{
    return g_sm.open ? (int)g_sm.step : -1;
}

void step_menu_owe_release(int step, uint64_t due_ms)
{
    sm_owe_release(&g_sm, step, due_ms);
}

/* SPI CALLBACK, after compaction: the step releases withheld from Move by
 * the tap guard (step_menu.h) that are now old enough to be a HOLD. */
uint32_t step_menu_take_due_releases(uint64_t now_ms)
{
    return sm_due_releases(&g_sm, now_ms);
}
